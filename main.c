#define _GNU_SOURCE
#include <cairo.h>
#include <ctype.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gdk/gdkwayland.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wayland-egl.h>

#define MAX_SW_W 1920
#define MAX_SW_H 1080
#define OVERLAY_HIDE_MS 2200
#define WHEEL_VOL_STEP 5.0
#define WHEEL_SEEK_STEP 5.0
#define VERT_SEEK_STEP 60.0
#define WHEEL_DEBOUNCE_US 20000

typedef struct {
    int width;
    int height;
    double volume;
    double seek_step;
    double volume_step;
    char vo[32];
    char hwdec[32];
    bool pause_on_start;
} AppConfig;

typedef struct {
    AppConfig cfg;

    GtkWidget *window;
    GtkWidget *overlay;
    GtkWidget *video_area;
    GtkWidget *top_bar;
    GtkWidget *bottom_box;
    GtkWidget *btn_play;
    GtkWidget *btn_open;
    GtkWidget *scale_seek;
    GtkWidget *scale_vol;
    GtkWidget *lbl_pos;
    GtkWidget *lbl_dur;
    GtkWidget *lbl_vol;

    gulong seek_changed_id;
    gulong vol_changed_id;
    guint hide_timeout_id;
    bool overlay_visible;
    bool pointer_over_seek;
    double smooth_scroll_acc;
    gint64 last_wheel_us;

    mpv_handle *mpv;
    mpv_render_context *mpv_ctx;
    bool use_gl;

    EGLDisplay egl_dpy;
    EGLContext egl_ctx;
    EGLSurface egl_pbuf;
    GLuint gl_fbo;
    GLuint gl_tex;
    int gl_w;
    int gl_h;
    uint8_t *gl_read_buf;

    cairo_surface_t *frame_surf;
    uint8_t *frame_buf;
    int frame_w;
    int frame_h;
    int frame_stride;

    pthread_t event_thread;
    pthread_mutex_t state_mtx;
    pthread_mutex_t frame_mtx;

    gint running;
    gint mpv_ready;
    gint render_pending;

    double duration;
    double position;
    double volume;
    bool paused;
    bool seeking_drag;
    guint idle_source_id;
} PlayerApp;

typedef struct {
    PlayerApp *app;
    char prop_name[16];
    int64_t track_id;
    bool is_off;
} TrackSelectData;

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

static bool parse_bool(const char *val, bool def) {
    if (strcasecmp(val, "1") == 0 || strcasecmp(val, "true") == 0 ||
        strcasecmp(val, "yes") == 0 || strcasecmp(val, "on") == 0) {
        return true;
    }
    if (strcasecmp(val, "0") == 0 || strcasecmp(val, "false") == 0 ||
        strcasecmp(val, "no") == 0 || strcasecmp(val, "off") == 0) {
        return false;
    }
    return def;
}

static void config_defaults(AppConfig *cfg) {
    cfg->width = 1024;
    cfg->height = 600;
    cfg->volume = 65.0;
    cfg->seek_step = 5.0;
    cfg->volume_step = 5.0;
    snprintf(cfg->vo, sizeof(cfg->vo), "gpu");
    snprintf(cfg->hwdec, sizeof(cfg->hwdec), "auto-safe");
    cfg->pause_on_start = false;
}

static void write_default_ini(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "[window]\n"
        "width = 1024\n"
        "height = 600\n\n"
        "[player]\n"
        "volume = 65\n"
        "seek_step = 5\n"
        "volume_step = 5\n"
        "vo = gpu\n"
        "hwdec = auto-safe\n"
        "pause_on_start = false\n"
    );
    fclose(f);
}

static char *resolve_config_path(const char *explicit_path) {
    if (explicit_path && *explicit_path) {
        return g_strdup(explicit_path);
    }
    const char *xdg = g_getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        return g_build_filename(xdg, "vd", "vd.ini", NULL);
    }
    const char *home = g_get_home_dir();
    return g_build_filename(home, ".config", "vd", "vd.ini", NULL);
}

static void load_config(PlayerApp *app, const char *explicit_path) {
    config_defaults(&app->cfg);
    char *path = resolve_config_path(explicit_path);
    if (!path) return;

    if (access(path, F_OK) != 0) {
        char *dir = g_path_get_dirname(path);
        g_mkdir_with_parents(dir, 0755);
        write_default_ini(path);
        g_free(dir);
    }

    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *comment = strpbrk(line, "#;");
            if (comment) *comment = '\0';
            char *s = trim_ws(line);
            if (!*s || *s == '[') continue;

            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = trim_ws(s);
            char *val = trim_ws(eq + 1);

            if (strcmp(key, "width") == 0) {
                int w = atoi(val);
                if (w >= 200 && w <= 16384) app->cfg.width = w;
            } else if (strcmp(key, "height") == 0) {
                int h = atoi(val);
                if (h >= 120 && h <= 16384) app->cfg.height = h;
            } else if (strcmp(key, "volume") == 0) {
                double v = atof(val);
                if (v >= 0.0 && v <= 100.0) app->cfg.volume = v;
            } else if (strcmp(key, "seek_step") == 0) {
                double step = atof(val);
                if (step > 0.0 && step <= 3600.0) app->cfg.seek_step = step;
            } else if (strcmp(key, "volume_step") == 0) {
                double vstep = atof(val);
                if (vstep > 0.0 && vstep <= 100.0) app->cfg.volume_step = vstep;
            } else if (strcmp(key, "vo") == 0 && *val) {
                snprintf(app->cfg.vo, sizeof(app->cfg.vo), "%s", val);
            } else if (strcmp(key, "hwdec") == 0 && *val) {
                snprintf(app->cfg.hwdec, sizeof(app->cfg.hwdec), "%s", val);
            } else if (strcmp(key, "pause_on_start") == 0) {
                app->cfg.pause_on_start = parse_bool(val, app->cfg.pause_on_start);
            }
        }
        fclose(f);
    }
    g_free(path);
}

static void format_time(double sec, char *buf, size_t n) {
    if (!isfinite(sec) || sec < 0.0) sec = 0.0;
    long total = (long)llround(sec);
    long h = total / 3600;
    long m = (total % 3600) / 60;
    long s = total % 60;
    if (h > 0) {
        snprintf(buf, n, "%ld:%02ld:%02ld", h, m, s);
    } else {
        snprintf(buf, n, "%02ld:%02ld", m, s);
    }
}

static void set_overlay_opacity(PlayerApp *app, double alpha) {
    if (app->top_bar) gtk_widget_set_opacity(app->top_bar, alpha);
    if (app->bottom_box) gtk_widget_set_opacity(app->bottom_box, alpha);
}

static gboolean hide_overlay_cb(gpointer user_data) {
    PlayerApp *app = user_data;
    app->hide_timeout_id = 0;

    pthread_mutex_lock(&app->state_mtx);
    bool dragging = app->seeking_drag;
    pthread_mutex_unlock(&app->state_mtx);

    if (dragging) {
        return G_SOURCE_REMOVE;
    }

    app->overlay_visible = false;
    set_overlay_opacity(app, 0.0);
    return G_SOURCE_REMOVE;
}

static void show_overlay_temporarily(PlayerApp *app) {
    if (!g_atomic_int_get(&app->running)) return;

    if (!app->overlay_visible) {
        app->overlay_visible = true;
        set_overlay_opacity(app, 1.0);
    }

    if (app->hide_timeout_id > 0) {
        g_source_remove(app->hide_timeout_id);
        app->hide_timeout_id = 0;
    }
    app->hide_timeout_id = g_timeout_add(OVERLAY_HIDE_MS, hide_overlay_cb, app);
}

static void destroy_frame_buffer_locked(PlayerApp *app) {
    if (app->frame_surf) {
        cairo_surface_destroy(app->frame_surf);
        app->frame_surf = NULL;
    }
    free(app->frame_buf);
    app->frame_buf = NULL;
    app->frame_w = 0;
    app->frame_h = 0;
    app->frame_stride = 0;
}

static bool ensure_frame_buffer_locked(PlayerApp *app, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (app->frame_surf && app->frame_w == w && app->frame_h == h) {
        return true;
    }

    destroy_frame_buffer_locked(app);

    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, w);
    if (stride <= 0) return false;

    uint8_t *buf = malloc((size_t)stride * (size_t)h);
    if (!buf) return false;

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        buf, CAIRO_FORMAT_RGB24, w, h, stride
    );
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        free(buf);
        return false;
    }

    app->frame_buf = buf;
    app->frame_surf = surf;
    app->frame_w = w;
    app->frame_h = h;
    app->frame_stride = stride;
    return true;
}

static void destroy_gl_fbo_locked(PlayerApp *app) {
    if (app->egl_dpy != EGL_NO_DISPLAY && app->egl_ctx != EGL_NO_CONTEXT) {
        eglMakeCurrent(app->egl_dpy, app->egl_pbuf, app->egl_pbuf, app->egl_ctx);
        if (app->gl_fbo) {
            glDeleteFramebuffers(1, &app->gl_fbo);
            app->gl_fbo = 0;
        }
        if (app->gl_tex) {
            glDeleteTextures(1, &app->gl_tex);
            app->gl_tex = 0;
        }
    }
    free(app->gl_read_buf);
    app->gl_read_buf = NULL;
    app->gl_w = 0;
    app->gl_h = 0;
}

static bool ensure_gl_fbo_locked(PlayerApp *app, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (app->gl_fbo && app->gl_w == w && app->gl_h == h) {
        return true;
    }

    destroy_gl_fbo_locked(app);

    eglMakeCurrent(app->egl_dpy, app->egl_pbuf, app->egl_pbuf, app->egl_ctx);

    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (tex) glDeleteTextures(1, &tex);
        return false;
    }

    size_t row_bytes = (size_t)w * 4;
    uint8_t *rbuf = malloc(row_bytes * (size_t)h);
    if (!rbuf) {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        return false;
    }

    app->gl_tex = tex;
    app->gl_fbo = fbo;
    app->gl_w = w;
    app->gl_h = h;
    app->gl_read_buf = rbuf;
    return true;
}

static void compute_target_size(int area_w, int area_h, int scale, int *out_w, int *out_h) {
    int pw = area_w * scale;
    int ph = area_h * scale;
    if (pw <= 0 || ph <= 0) {
        *out_w = 0;
        *out_h = 0;
        return;
    }

    double sx = (pw > MAX_SW_W) ? ((double)MAX_SW_W / (double)pw) : 1.0;
    double sy = (ph > MAX_SW_H) ? ((double)MAX_SW_H / (double)ph) : 1.0;
    double s = (sx < sy) ? sx : sy;

    int tw = (int)llround((double)pw * s);
    int th = (int)llround((double)ph * s);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    *out_w = tw;
    *out_h = th;
}

static gboolean render_idle(gpointer user_data) {
    PlayerApp *app = user_data;
    g_atomic_int_set(&app->render_pending, 0);

    if (!g_atomic_int_get(&app->running)) {
        return G_SOURCE_REMOVE;
    }

    if (!app->mpv_ctx || !app->video_area || !gtk_widget_get_realized(app->video_area)) {
        return G_SOURCE_REMOVE;
    }

    int w = gtk_widget_get_allocated_width(app->video_area);
    int h = gtk_widget_get_allocated_height(app->video_area);
    int scale = gtk_widget_get_scale_factor(app->video_area);
    int pw = 0, ph = 0;
    compute_target_size(w, h, scale, &pw, &ph);
    if (pw <= 0 || ph <= 0) return G_SOURCE_REMOVE;

    if (app->use_gl) {
        pthread_mutex_lock(&app->frame_mtx);
        if (ensure_gl_fbo_locked(app, pw, ph) && ensure_frame_buffer_locked(app, pw, ph)) {
            eglMakeCurrent(app->egl_dpy, app->egl_pbuf, app->egl_pbuf, app->egl_ctx);
            mpv_opengl_fbo fbo_param = {
                .fbo = (int)app->gl_fbo,
                .w = pw,
                .h = ph,
                .internal_format = GL_RGBA8,
            };
            int flip_y = 1;
            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_OPENGL_FBO, &fbo_param},
                {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
                {0, NULL}
            };
            mpv_render_context_render(app->mpv_ctx, params);

            glBindFramebuffer(GL_READ_FRAMEBUFFER, app->gl_fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, pw, ph, GL_BGRA, GL_UNSIGNED_BYTE, app->gl_read_buf);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

            for (int y = 0; y < ph; y++) {
                memcpy(
                    app->frame_buf + (size_t)y * (size_t)app->frame_stride,
                    app->gl_read_buf + (size_t)y * (size_t)pw * 4,
                    (size_t)pw * 4
                );
            }
            cairo_surface_mark_dirty(app->frame_surf);
        }
        pthread_mutex_unlock(&app->frame_mtx);
    } else {
        pthread_mutex_lock(&app->frame_mtx);
        if (ensure_frame_buffer_locked(app, pw, ph)) {
            int size[2] = {pw, ph};
            size_t stride = (size_t)app->frame_stride;
            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_SW_SIZE, size},
                {MPV_RENDER_PARAM_SW_FORMAT, "bgr0"},
                {MPV_RENDER_PARAM_SW_STRIDE, &stride},
                {MPV_RENDER_PARAM_SW_POINTER, app->frame_buf},
                {0, NULL}
            };
            mpv_render_context_render(app->mpv_ctx, params);
            cairo_surface_mark_dirty(app->frame_surf);
        }
        pthread_mutex_unlock(&app->frame_mtx);
    }

    gtk_widget_queue_draw(app->video_area);
    return G_SOURCE_REMOVE;
}

static void schedule_render(PlayerApp *app) {
    if (!g_atomic_int_get(&app->running)) return;
    if (g_atomic_int_compare_and_exchange(&app->render_pending, 0, 1)) {
        g_idle_add(render_idle, app);
    }
}

static void on_mpv_render_update(void *cb_ctx) {
    PlayerApp *app = cb_ctx;
    if (!g_atomic_int_get(&app->running)) return;
    schedule_render(app);
}

static gboolean on_video_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    PlayerApp *app = user_data;
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);

    cairo_set_source_rgb(cr, 0.06, 0.06, 0.06);
    cairo_paint(cr);

    pthread_mutex_lock(&app->frame_mtx);
    if (app->frame_surf && app->frame_w > 0 && app->frame_h > 0) {
        double sx = (double)w / (double)app->frame_w;
        double sy = (double)h / (double)app->frame_h;

        cairo_save(cr);
        cairo_translate(cr, 0.0, (double)h);
        cairo_scale(cr, sx, -sy);
        cairo_set_source_surface(cr, app->frame_surf, 0.0, 0.0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    pthread_mutex_unlock(&app->frame_mtx);

    return TRUE;
}

static void on_video_size_allocate(GtkWidget *widget, GtkAllocation *alloc, gpointer user_data) {
    (void)widget;
    (void)alloc;
    PlayerApp *app = user_data;
    schedule_render(app);
}

static gboolean ui_sync_callback(gpointer user_data) {
    PlayerApp *app = user_data;
    if (!g_atomic_int_get(&app->running)) {
        app->idle_source_id = 0;
        return G_SOURCE_REMOVE;
    }

    pthread_mutex_lock(&app->state_mtx);
    double pos = app->position;
    double dur = app->duration;
    double vol = app->volume;
    bool paused = app->paused;
    bool dragging = app->seeking_drag;
    pthread_mutex_unlock(&app->state_mtx);

    if (app->btn_play) {
        gtk_button_set_label(GTK_BUTTON(app->btn_play), paused ? "Play" : "Pause");
    }

    if (!dragging) {
        if (app->seek_changed_id > 0) {
            g_signal_handler_block(app->scale_seek, app->seek_changed_id);
        }
        GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(app->scale_seek));
        double upper = (dur > 0.0) ? dur : 100.0;
        gtk_adjustment_configure(adj,
            (dur > 0.0) ? pos : 0.0,
            0.0, upper, 1.0, 10.0, 0.0);
        gtk_widget_set_sensitive(app->scale_seek, dur > 0.0);
        if (app->seek_changed_id > 0) {
            g_signal_handler_unblock(app->scale_seek, app->seek_changed_id);
        }
    }

    char cur_str[32], dur_str[32];
    format_time(pos, cur_str, sizeof(cur_str));
    format_time(dur, dur_str, sizeof(dur_str));
    gtk_label_set_text(GTK_LABEL(app->lbl_pos), cur_str);
    gtk_label_set_text(GTK_LABEL(app->lbl_dur), dur_str);

    if (vol < 0.0) vol = 0.0;
    if (vol > 100.0) vol = 100.0;

    char vol_buf[32];
    snprintf(vol_buf, sizeof(vol_buf), "%.0f%%", vol);
    gtk_label_set_text(GTK_LABEL(app->lbl_vol), vol_buf);

    double cur_vol = gtk_range_get_value(GTK_RANGE(app->scale_vol));
    if (fabs(cur_vol - vol) > 0.05) {
        if (app->vol_changed_id > 0) {
            g_signal_handler_block(app->scale_vol, app->vol_changed_id);
        }
        gtk_range_set_value(GTK_RANGE(app->scale_vol), vol);
        if (app->vol_changed_id > 0) {
            g_signal_handler_unblock(app->scale_vol, app->vol_changed_id);
        }
    }

    app->idle_source_id = 0;
    return G_SOURCE_REMOVE;
}

static void schedule_ui_sync(PlayerApp *app) {
    if (!g_atomic_int_get(&app->running)) return;
    if (app->idle_source_id == 0) {
        app->idle_source_id = g_idle_add(ui_sync_callback, app);
    }
}

static void *mpv_event_thread(void *arg) {
    PlayerApp *app = arg;

    while (g_atomic_int_get(&app->running)) {
        mpv_event *ev = mpv_wait_event(app->mpv, 0.25);
        if (!ev) continue;
        if (ev->event_id == MPV_EVENT_NONE) continue;

        bool ui_dirty = false;
        bool redraw = false;

        pthread_mutex_lock(&app->state_mtx);
        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *prop = ev->data;
            if (strcmp(prop->name, "time-pos") == 0) {
                if (prop->format == MPV_FORMAT_DOUBLE) {
                    app->position = *(double *)prop->data;
                } else if (prop->format == MPV_FORMAT_NONE) {
                    app->position = 0.0;
                }
                ui_dirty = true;
            } else if (strcmp(prop->name, "duration") == 0) {
                if (prop->format == MPV_FORMAT_DOUBLE) {
                    app->duration = *(double *)prop->data;
                } else if (prop->format == MPV_FORMAT_NONE) {
                    app->duration = 0.0;
                }
                ui_dirty = true;
            } else if (strcmp(prop->name, "pause") == 0) {
                if (prop->format == MPV_FORMAT_FLAG) {
                    app->paused = (*(int *)prop->data) != 0;
                    ui_dirty = true;
                }
            } else if (strcmp(prop->name, "volume") == 0) {
                if (prop->format == MPV_FORMAT_DOUBLE) {
                    double v = *(double *)prop->data;
                    if (v < 0.0) v = 0.0;
                    if (v > 100.0) v = 100.0;
                    app->volume = v;
                    ui_dirty = true;
                }
            }
        } else if (ev->event_id == MPV_EVENT_FILE_LOADED) {
            ui_dirty = true;
            redraw = true;
        } else if (ev->event_id == MPV_EVENT_END_FILE) {
            ui_dirty = true;
        }
        pthread_mutex_unlock(&app->state_mtx);

        if (redraw) {
            schedule_render(app);
        }
        if (ui_dirty) {
            schedule_ui_sync(app);
        }
    }

    return NULL;
}

static void load_media(PlayerApp *app, const char *path) {
    if (!path || !*path || !app->mpv) return;
    const char *cmd[] = {"loadfile", path, "replace", NULL};
    mpv_command_async(app->mpv, 0, cmd);
}

static void toggle_pause(PlayerApp *app) {
    if (!app->mpv) return;
    const char *cmd[] = {"cycle", "pause", NULL};
    mpv_command_async(app->mpv, 0, cmd);
}

static void seek_relative(PlayerApp *app, double delta) {
    if (!app->mpv) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f", delta);
    const char *cmd[] = {"seek", buf, "relative", NULL};
    mpv_command_async(app->mpv, 0, cmd);
}

static void adjust_volume(PlayerApp *app, double delta) {
    if (!app->mpv) return;
    pthread_mutex_lock(&app->state_mtx);
    double next = app->volume + delta;
    if (next < 0.0) next = 0.0;
    if (next > 100.0) next = 100.0;
    app->volume = next;
    pthread_mutex_unlock(&app->state_mtx);

    mpv_set_property_async(app->mpv, 0, "volume", MPV_FORMAT_DOUBLE, &next);
    schedule_ui_sync(app);
}

static void on_track_item_activate(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    TrackSelectData *sel = user_data;
    if (!sel || !sel->app || !sel->app->mpv) return;

    if (sel->is_off) {
        const char *off_val = "no";
        mpv_set_property_async(sel->app->mpv, 0, sel->prop_name, MPV_FORMAT_STRING, &off_val);
    } else {
        int64_t tid = sel->track_id;
        mpv_set_property_async(sel->app->mpv, 0, sel->prop_name, MPV_FORMAT_INT64, &tid);
    }
    show_overlay_temporarily(sel->app);
}

static void free_track_select_data(gpointer data, GClosure *closure) {
    (void)closure;
    free(data);
}

static void add_section_header(GtkWidget *menu, const char *title) {
    GtkWidget *header = gtk_menu_item_new_with_label(title);
    gtk_widget_set_sensitive(header, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), header);
}

static void add_track_choice_item(GtkWidget *menu, PlayerApp *app, const char *label,
                                  const char *prop_name, int64_t track_id,
                                  bool is_off, bool is_selected) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s %s", is_selected ? "●" : "○", label);

    GtkWidget *item = gtk_menu_item_new_with_label(buf);
    TrackSelectData *sel = calloc(1, sizeof(*sel));
    if (sel) {
        sel->app = app;
        snprintf(sel->prop_name, sizeof(sel->prop_name), "%s", prop_name);
        sel->track_id = track_id;
        sel->is_off = is_off;
        g_signal_connect_data(item, "activate",
            G_CALLBACK(on_track_item_activate), sel, free_track_select_data, 0);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static void populate_tracks_for_type(GtkWidget *menu, PlayerApp *app, mpv_node *list_node,
                                   const char *section_title, const char *track_type,
                                   const char *prop_name, bool allow_off, bool always_show_section) {
    int count = 0;
    bool any_selected = false;

    if (always_show_section) {
        if (g_list_length(gtk_container_get_children(GTK_CONTAINER(menu))) > 0) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        }
        add_section_header(menu, section_title);
    }

    if (list_node && list_node->format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < list_node->u.list->num; i++) {
            mpv_node *tr = &list_node->u.list->values[i];
            if (tr->format != MPV_FORMAT_NODE_MAP) continue;

            const char *type = NULL;
            int64_t id = -1;
            const char *title = NULL;
            const char *lang = NULL;
            const char *codec = NULL;
            bool selected = false;

            for (int k = 0; k < tr->u.list->num; k++) {
                const char *key = tr->u.list->keys[k];
                mpv_node *val = &tr->u.list->values[k];
                if (!key || !val) continue;

                if (strcmp(key, "type") == 0 && val->format == MPV_FORMAT_STRING) {
                    type = val->u.string;
                } else if (strcmp(key, "id") == 0 && val->format == MPV_FORMAT_INT64) {
                    id = val->u.int64;
                } else if (strcmp(key, "title") == 0 && val->format == MPV_FORMAT_STRING) {
                    title = val->u.string;
                } else if (strcmp(key, "lang") == 0 && val->format == MPV_FORMAT_STRING) {
                    lang = val->u.string;
                } else if (strcmp(key, "codec") == 0 && val->format == MPV_FORMAT_STRING) {
                    codec = val->u.string;
                } else if (strcmp(key, "selected") == 0 && val->format == MPV_FORMAT_FLAG) {
                    selected = val->u.flag != 0;
                }
            }

            if (!type || strcmp(type, track_type) != 0 || id < 0) continue;

            if (!always_show_section && count == 0) {
                if (g_list_length(gtk_container_get_children(GTK_CONTAINER(menu))) > 0) {
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
                }
                add_section_header(menu, section_title);
            }

            count++;
            if (selected) any_selected = true;

            char label[220];
            if (title && *title && lang && *lang) {
                snprintf(label, sizeof(label), "#%" PRId64 " %s [%s]%s%s",
                    id, title, lang, codec ? " · " : "", codec ? codec : "");
            } else if (title && *title) {
                snprintf(label, sizeof(label), "#%" PRId64 " %s%s%s",
                    id, title, codec ? " · " : "", codec ? codec : "");
            } else if (lang && *lang) {
                snprintf(label, sizeof(label), "#%" PRId64 " [%s]%s%s",
                    id, lang, codec ? " · " : "", codec ? codec : "");
            } else {
                snprintf(label, sizeof(label), "#%" PRId64 "%s%s",
                    id, codec ? " · " : "", codec ? codec : "");
            }

            add_track_choice_item(menu, app, label, prop_name, id, false, selected);
        }
    }

    if (allow_off && (count > 0 || always_show_section)) {
        add_track_choice_item(menu, app, "Off", prop_name, 0, true, !any_selected);
    }
}

static void open_track_menu(PlayerApp *app) {
    if (!app->mpv || !app->window) return;

    mpv_node tracks_node;
    memset(&tracks_node, 0, sizeof(tracks_node));
    int err = mpv_get_property(app->mpv, "track-list", MPV_FORMAT_NODE, &tracks_node);

    GtkWidget *menu = gtk_menu_new();

    if (err >= 0) {
        populate_tracks_for_type(menu, app, &tracks_node, "Audio", "audio", "aid", true, false);
        populate_tracks_for_type(menu, app, &tracks_node, "Subtitles", "sub", "sid", true, true);
        populate_tracks_for_type(menu, app, &tracks_node, "Video", "video", "vid", false, false);
        mpv_free_node_contents(&tracks_node);
    } else {
        populate_tracks_for_type(menu, app, NULL, "Subtitles", "sub", "sid", true, true);
    }

    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), app->window, NULL);
    gtk_menu_popup_at_widget(
        GTK_MENU(menu),
        app->window,
        GDK_GRAVITY_CENTER,
        GDK_GRAVITY_CENTER,
        NULL
    );
    show_overlay_temporarily(app);
}

static void on_btn_open_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    PlayerApp *app = user_data;

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Open Video",
        GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT,
        NULL
    );

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        load_media(app, filename);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static gboolean on_seek_press(GtkWidget *w, GdkEventButton *ev, gpointer user_data) {
    (void)w;
    if (ev && ev->button == GDK_BUTTON_SECONDARY) {
        return FALSE;
    }
    PlayerApp *app = user_data;
    pthread_mutex_lock(&app->state_mtx);
    app->seeking_drag = true;
    pthread_mutex_unlock(&app->state_mtx);
    show_overlay_temporarily(app);
    return FALSE;
}

static gboolean on_seek_release(GtkWidget *w, GdkEventButton *ev, gpointer user_data) {
    if (ev && ev->button == GDK_BUTTON_SECONDARY) {
        return FALSE;
    }
    PlayerApp *app = user_data;
    double target = gtk_range_get_value(GTK_RANGE(w));

    pthread_mutex_lock(&app->state_mtx);
    app->seeking_drag = false;
    app->position = target;
    pthread_mutex_unlock(&app->state_mtx);

    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f", target);
    const char *cmd[] = {"seek", buf, "absolute", NULL};
    mpv_command_async(app->mpv, 0, cmd);
    schedule_ui_sync(app);
    show_overlay_temporarily(app);
    return FALSE;
}

static void on_seek_value_changed(GtkRange *range, gpointer user_data) {
    PlayerApp *app = user_data;
    double val = gtk_range_get_value(range);

    char cur_str[32];
    pthread_mutex_lock(&app->state_mtx);
    double dur = app->duration;
    bool dragging = app->seeking_drag;
    pthread_mutex_unlock(&app->state_mtx);

    format_time(val, cur_str, sizeof(cur_str));
    gtk_label_set_text(GTK_LABEL(app->lbl_pos), cur_str);

    if (!dragging && dur > 0.0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.3f", val);
        const char *cmd[] = {"seek", buf, "absolute", NULL};
        mpv_command_async(app->mpv, 0, cmd);
    }
}

static bool is_pointer_over_seek(PlayerApp *app) {
    if (app->pointer_over_seek) return true;
    if (!app->scale_seek || !gtk_widget_get_realized(app->scale_seek)) return false;

    GtkAllocation alloc;
    gtk_widget_get_allocation(app->scale_seek, &alloc);
    if (alloc.width <= 0 || alloc.height <= 0) return false;

    GdkWindow *win = gtk_widget_get_window(app->window);
    if (!win) return false;

    GdkDisplay *dpy = gdk_window_get_display(win);
    GdkSeat *seat = gdk_display_get_default_seat(dpy);
    GdkDevice *ptr = seat ? gdk_seat_get_pointer(seat) : NULL;
    if (!ptr) return false;

    int wx = 0, wy = 0;
    gdk_window_get_device_position(win, ptr, &wx, &wy, NULL);

    int sx = 0, sy = 0;
    if (gtk_widget_translate_coordinates(app->scale_seek, app->window, 0, 0, &sx, &sy)) {
        if (wx >= sx && wx <= sx + alloc.width && wy >= sy && wy <= sy + alloc.height) {
            return true;
        }
    }
    return false;
}

static gboolean on_seek_enter(GtkWidget *w, GdkEventCrossing *ev, gpointer user_data) {
    (void)w;
    (void)ev;
    PlayerApp *app = user_data;
    app->pointer_over_seek = true;
    show_overlay_temporarily(app);
    return FALSE;
}

static gboolean on_seek_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer user_data) {
    (void)w;
    (void)ev;
    PlayerApp *app = user_data;
    app->pointer_over_seek = false;
    return FALSE;
}

static int extract_wheel_notches(PlayerApp *app, GdkEventScroll *ev) {
    if (!ev) return 0;

    int notches = 0;
    if (ev->direction == GDK_SCROLL_UP) {
        notches = 1;
        app->smooth_scroll_acc = 0.0;
    } else if (ev->direction == GDK_SCROLL_DOWN) {
        notches = -1;
        app->smooth_scroll_acc = 0.0;
    } else if (ev->direction == GDK_SCROLL_SMOOTH) {
        app->smooth_scroll_acc += -ev->delta_y;
        while (app->smooth_scroll_acc >= 0.5) {
            notches += 1;
            app->smooth_scroll_acc -= 0.5;
        }
        while (app->smooth_scroll_acc <= -0.5) {
            notches -= 1;
            app->smooth_scroll_acc += 0.5;
        }
    }

    if (notches != 0) {
        gint64 now = g_get_monotonic_time();
        if (app->last_wheel_us != 0 && (now - app->last_wheel_us) < WHEEL_DEBOUNCE_US) {
            return 0;
        }
        app->last_wheel_us = now;
        return (notches > 0) ? 1 : -1;
    }
    return 0;
}

static gboolean on_seek_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer user_data) {
    (void)w;
    PlayerApp *app = user_data;
    int dir = extract_wheel_notches(app, ev);
    if (dir != 0) {
        seek_relative(app, WHEEL_SEEK_STEP * (double)dir);
        show_overlay_temporarily(app);
    }
    return TRUE;
}

static gboolean handle_wheel_volume(PlayerApp *app, GdkEventScroll *ev) {
    if (is_pointer_over_seek(app)) {
        int dir = extract_wheel_notches(app, ev);
        if (dir != 0) {
            seek_relative(app, WHEEL_SEEK_STEP * (double)dir);
            show_overlay_temporarily(app);
        }
        return TRUE;
    }

    int dir = extract_wheel_notches(app, ev);
    if (dir != 0) {
        adjust_volume(app, WHEEL_VOL_STEP * (double)dir);
        show_overlay_temporarily(app);
    }
    return TRUE;
}

static gboolean on_vol_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer user_data) {
    (void)w;
    PlayerApp *app = user_data;
    int dir = extract_wheel_notches(app, ev);
    if (dir != 0) {
        adjust_volume(app, WHEEL_VOL_STEP * (double)dir);
        show_overlay_temporarily(app);
    }
    return TRUE;
}

static void on_vol_value_changed(GtkRange *range, gpointer user_data) {
    PlayerApp *app = user_data;
    double vol = gtk_range_get_value(range);
    if (vol < 0.0) vol = 0.0;
    if (vol > 100.0) vol = 100.0;

    pthread_mutex_lock(&app->state_mtx);
    app->volume = vol;
    pthread_mutex_unlock(&app->state_mtx);

    mpv_set_property_async(app->mpv, 0, "volume", MPV_FORMAT_DOUBLE, &vol);
    char vol_buf[32];
    snprintf(vol_buf, sizeof(vol_buf), "%.0f%%", vol);
    gtk_label_set_text(GTK_LABEL(app->lbl_vol), vol_buf);
    show_overlay_temporarily(app);
}

static gboolean on_motion_notify(GtkWidget *w, GdkEventMotion *ev, gpointer user_data) {
    (void)w;
    (void)ev;
    PlayerApp *app = user_data;
    show_overlay_temporarily(app);
    return FALSE;
}

static gboolean on_window_button_press(GtkWidget *w, GdkEventButton *ev, gpointer user_data) {
    (void)w;
    if (!ev) return FALSE;
    if (ev->type == GDK_BUTTON_PRESS && ev->button == GDK_BUTTON_SECONDARY) {
        PlayerApp *app = user_data;
        toggle_pause(app);
        show_overlay_temporarily(app);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_window_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer user_data) {
    (void)w;
    PlayerApp *app = user_data;
    return handle_wheel_volume(app, ev);
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer user_data) {
    (void)w;
    PlayerApp *app = user_data;

    switch (ev->keyval) {
        case GDK_KEY_space:
        case GDK_KEY_p:
            toggle_pause(app);
            show_overlay_temporarily(app);
            return TRUE;
        case GDK_KEY_Left:
            seek_relative(app, -app->cfg.seek_step);
            show_overlay_temporarily(app);
            return TRUE;
        case GDK_KEY_Right:
            seek_relative(app, app->cfg.seek_step);
            show_overlay_temporarily(app);
            return TRUE;
        case GDK_KEY_Down:
            seek_relative(app, -VERT_SEEK_STEP);
            show_overlay_temporarily(app);
            return TRUE;
        case GDK_KEY_Up:
            seek_relative(app, VERT_SEEK_STEP);
            show_overlay_temporarily(app);
            return TRUE;
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract:
            adjust_volume(app, -app->cfg.volume_step);
            show_overlay_temporarily(app);
            return TRUE;
        case GDK_KEY_equal:
        case GDK_KEY_plus:
        case GDK_KEY_KP_Add:
            adjust_volume(app, app->cfg.volume_step);
            show_overlay_temporarily(app);
            return TRUE;
        case GDK_KEY_c:
        case GDK_KEY_C:
            open_track_menu(app);
            return TRUE;
        case GDK_KEY_o:
            on_btn_open_clicked(NULL, app);
            show_overlay_temporarily(app);
            return TRUE;
        case GDK_KEY_q:
            gtk_widget_destroy(app->window);
            return TRUE;
        default:
            return FALSE;
    }
}

static void on_window_destroy(GtkWidget *w, gpointer user_data) {
    (void)w;
    PlayerApp *app = user_data;
    g_atomic_int_set(&app->running, 0);
    gtk_main_quit();
}

static void apply_overlay_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css =
        ".vd-hud-panel {"
        "  background-color: rgba(24, 24, 24, 0.72);"
        "  border-radius: 8px;"
        "  padding: 6px 10px;"
        "}"
        ".vd-hud-panel label {"
        "  color: #eeeeee;"
        "}";
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);
}

static void build_ui(PlayerApp *app) {
    apply_overlay_css();

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "vd");
    gtk_window_set_default_size(GTK_WINDOW(app->window), app->cfg.width, app->cfg.height);
    gtk_widget_add_events(app->window,
        GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_window_destroy), app);
    g_signal_connect(app->window, "key-press-event", G_CALLBACK(on_key_press), app);
    g_signal_connect(app->window, "motion-notify-event", G_CALLBACK(on_motion_notify), app);
    g_signal_connect(app->window, "button-press-event", G_CALLBACK(on_window_button_press), app);
    g_signal_connect(app->window, "scroll-event", G_CALLBACK(on_window_scroll), app);

    app->overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(app->window), app->overlay);

    app->video_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(app->video_area, TRUE);
    gtk_widget_set_vexpand(app->video_area, TRUE);
    g_signal_connect(app->video_area, "draw", G_CALLBACK(on_video_draw), app);
    g_signal_connect(app->video_area, "size-allocate", G_CALLBACK(on_video_size_allocate), app);
    gtk_container_add(GTK_CONTAINER(app->overlay), app->video_area);

    app->top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(app->top_bar, GTK_ALIGN_END);
    gtk_widget_set_valign(app->top_bar, GTK_ALIGN_START);
    gtk_widget_set_margin_top(app->top_bar, 10);
    gtk_widget_set_margin_end(app->top_bar, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(app->top_bar), "vd-hud-panel");

    double init_vol = app->cfg.volume;
    if (init_vol < 0.0) init_vol = 0.0;
    if (init_vol > 100.0) init_vol = 100.0;

    char vol_init[32];
    snprintf(vol_init, sizeof(vol_init), "%.0f%%", init_vol);
    app->lbl_vol = gtk_label_new(vol_init);
    gtk_label_set_width_chars(GTK_LABEL(app->lbl_vol), 4);
    gtk_label_set_xalign(GTK_LABEL(app->lbl_vol), 1.0);
    gtk_widget_set_size_request(app->lbl_vol, 42, -1);

    app->scale_vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(app->scale_vol), FALSE);
    gtk_widget_set_size_request(app->scale_vol, 110, -1);
    gtk_widget_add_events(app->scale_vol, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    gtk_range_set_value(GTK_RANGE(app->scale_vol), init_vol);
    g_signal_connect(app->scale_vol, "scroll-event", G_CALLBACK(on_vol_scroll), app);
    app->vol_changed_id = g_signal_connect(app->scale_vol, "value-changed", G_CALLBACK(on_vol_value_changed), app);

    gtk_box_pack_start(GTK_BOX(app->top_bar), app->scale_vol, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app->top_bar), app->lbl_vol, FALSE, FALSE, 2);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->overlay), app->top_bar);

    app->bottom_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_halign(app->bottom_box, GTK_ALIGN_FILL);
    gtk_widget_set_valign(app->bottom_box, GTK_ALIGN_END);
    gtk_widget_set_margin_start(app->bottom_box, 12);
    gtk_widget_set_margin_end(app->bottom_box, 12);
    gtk_widget_set_margin_bottom(app->bottom_box, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(app->bottom_box), "vd-hud-panel");

    app->scale_seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(app->scale_seek), FALSE);
    gtk_widget_set_sensitive(app->scale_seek, FALSE);
    gtk_widget_add_events(app->scale_seek,
        GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(app->scale_seek, "button-press-event", G_CALLBACK(on_seek_press), app);
    g_signal_connect(app->scale_seek, "button-release-event", G_CALLBACK(on_seek_release), app);
    g_signal_connect(app->scale_seek, "scroll-event", G_CALLBACK(on_seek_scroll), app);
    g_signal_connect(app->scale_seek, "enter-notify-event", G_CALLBACK(on_seek_enter), app);
    g_signal_connect(app->scale_seek, "leave-notify-event", G_CALLBACK(on_seek_leave), app);
    app->seek_changed_id = g_signal_connect(app->scale_seek, "value-changed", G_CALLBACK(on_seek_value_changed), app);
    gtk_box_pack_start(GTK_BOX(app->bottom_box), app->scale_seek, FALSE, FALSE, 0);

    GtkWidget *time_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(app->bottom_box), time_row, FALSE, FALSE, 0);

    app->lbl_pos = gtk_label_new("00:00");
    gtk_box_pack_start(GTK_BOX(time_row), app->lbl_pos, FALSE, FALSE, 0);

    app->lbl_dur = gtk_label_new("00:00");
    gtk_box_pack_end(GTK_BOX(time_row), app->lbl_dur, FALSE, FALSE, 0);

    gtk_overlay_add_overlay(GTK_OVERLAY(app->overlay), app->bottom_box);

    app->overlay_visible = true;
    show_overlay_temporarily(app);
}

static void *get_egl_proc_address(void *ctx, const char *name) {
    (void)ctx;
    return (void *)eglGetProcAddress(name);
}

static bool init_egl_gl(PlayerApp *app) {
    app->egl_dpy = EGL_NO_DISPLAY;
    app->egl_ctx = EGL_NO_CONTEXT;
    app->egl_pbuf = EGL_NO_SURFACE;

    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!gdk_dpy || !GDK_IS_WAYLAND_DISPLAY(gdk_dpy)) {
        return false;
    }

    struct wl_display *wl_dpy = gdk_wayland_display_get_wl_display(gdk_dpy);
    if (!wl_dpy) return false;

    app->egl_dpy = eglGetDisplay((EGLNativeDisplayType)wl_dpy);
    if (app->egl_dpy == EGL_NO_DISPLAY) {
        app->egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (app->egl_dpy == EGL_NO_DISPLAY) return false;

    EGLint major = 0, minor = 0;
    if (!eglInitialize(app->egl_dpy, &major, &minor)) {
        app->egl_dpy = EGL_NO_DISPLAY;
        return false;
    }

    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    EGLConfig cfg = NULL;
    EGLint num_cfg = 0;
    if (!eglChooseConfig(app->egl_dpy, cfg_attribs, &cfg, 1, &num_cfg) || num_cfg < 1) {
        eglTerminate(app->egl_dpy);
        app->egl_dpy = EGL_NO_DISPLAY;
        return false;
    }

    eglBindAPI(EGL_OPENGL_API);
    app->egl_ctx = eglCreateContext(app->egl_dpy, cfg, EGL_NO_CONTEXT, NULL);
    if (app->egl_ctx == EGL_NO_CONTEXT) {
        eglTerminate(app->egl_dpy);
        app->egl_dpy = EGL_NO_DISPLAY;
        return false;
    }

    EGLint pbuf_attribs[] = {
        EGL_WIDTH, 16,
        EGL_HEIGHT, 16,
        EGL_NONE
    };
    app->egl_pbuf = eglCreatePbufferSurface(app->egl_dpy, cfg, pbuf_attribs);
    if (app->egl_pbuf == EGL_NO_SURFACE) {
        eglDestroyContext(app->egl_dpy, app->egl_ctx);
        eglTerminate(app->egl_dpy);
        app->egl_ctx = EGL_NO_CONTEXT;
        app->egl_dpy = EGL_NO_DISPLAY;
        return false;
    }

    if (!eglMakeCurrent(app->egl_dpy, app->egl_pbuf, app->egl_pbuf, app->egl_ctx)) {
        eglDestroySurface(app->egl_dpy, app->egl_pbuf);
        eglDestroyContext(app->egl_dpy, app->egl_ctx);
        eglTerminate(app->egl_dpy);
        app->egl_pbuf = EGL_NO_SURFACE;
        app->egl_ctx = EGL_NO_CONTEXT;
        app->egl_dpy = EGL_NO_DISPLAY;
        return false;
    }

    return true;
}

static void destroy_egl_gl(PlayerApp *app) {
    destroy_gl_fbo_locked(app);
    if (app->egl_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(app->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (app->egl_pbuf != EGL_NO_SURFACE) {
            eglDestroySurface(app->egl_dpy, app->egl_pbuf);
            app->egl_pbuf = EGL_NO_SURFACE;
        }
        if (app->egl_ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(app->egl_dpy, app->egl_ctx);
            app->egl_ctx = EGL_NO_CONTEXT;
        }
        eglTerminate(app->egl_dpy);
        app->egl_dpy = EGL_NO_DISPLAY;
    }
}

static bool init_mpv(PlayerApp *app) {
    app->mpv = mpv_create();
    if (!app->mpv) return false;

    double init_vol = app->cfg.volume;
    if (init_vol < 0.0) init_vol = 0.0;
    if (init_vol > 100.0) init_vol = 100.0;

    int pause_flag = app->cfg.pause_on_start ? 1 : 0;
    mpv_set_option_string(app->mpv, "vo", "libmpv");
    mpv_set_option_string(app->mpv, "hwdec", app->cfg.hwdec);
    mpv_set_option_string(app->mpv, "vd-lavc-threads", "0");
    mpv_set_option_string(app->mpv, "sub-auto", "fuzzy");
    mpv_set_option(app->mpv, "pause", MPV_FORMAT_FLAG, &pause_flag);
    mpv_set_option(app->mpv, "volume", MPV_FORMAT_DOUBLE, &init_vol);
    mpv_set_option_string(app->mpv, "keep-open", "yes");

    if (mpv_initialize(app->mpv) < 0) {
        mpv_terminate_destroy(app->mpv);
        app->mpv = NULL;
        return false;
    }

    if (init_egl_gl(app)) {
        mpv_opengl_init_params gl_init = {
            .get_proc_address = get_egl_proc_address,
            .get_proc_address_ctx = NULL,
        };
        mpv_render_param gl_params[] = {
            {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
            {0, NULL}
        };
        if (mpv_render_context_create(&app->mpv_ctx, app->mpv, gl_params) >= 0) {
            app->use_gl = true;
        } else {
            destroy_egl_gl(app);
            app->use_gl = false;
        }
    }

    if (!app->use_gl) {
        mpv_render_param sw_params[] = {
            {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_SW},
            {0, NULL}
        };
        if (mpv_render_context_create(&app->mpv_ctx, app->mpv, sw_params) < 0) {
            mpv_terminate_destroy(app->mpv);
            app->mpv = NULL;
            return false;
        }
    }

    mpv_render_context_set_update_callback(app->mpv_ctx, on_mpv_render_update, app);

    mpv_observe_property(app->mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(app->mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(app->mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(app->mpv, 0, "volume", MPV_FORMAT_DOUBLE);

    app->volume = init_vol;
    app->paused = app->cfg.pause_on_start;
    g_atomic_int_set(&app->mpv_ready, 1);
    return true;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    PlayerApp app;
    memset(&app, 0, sizeof(app));
    pthread_mutex_init(&app.state_mtx, NULL);
    pthread_mutex_init(&app.frame_mtx, NULL);
    g_atomic_int_set(&app.running, 1);

    const char *cfg_arg = NULL;
    const char *media_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            cfg_arg = argv[++i];
        } else if (argv[i][0] != '-') {
            media_arg = argv[i];
        }
    }

    load_config(&app, cfg_arg);
    build_ui(&app);
    gtk_widget_show_all(app.window);

    if (!init_mpv(&app)) {
        fprintf(stderr, "Failed to initialize libmpv render context\n");
        pthread_mutex_destroy(&app.frame_mtx);
        pthread_mutex_destroy(&app.state_mtx);
        return 1;
    }

    if (pthread_create(&app.event_thread, NULL, mpv_event_thread, &app) != 0) {
        fprintf(stderr, "Failed to create event thread\n");
        if (app.mpv_ctx) mpv_render_context_free(app.mpv_ctx);
        destroy_egl_gl(&app);
        mpv_terminate_destroy(app.mpv);
        pthread_mutex_destroy(&app.frame_mtx);
        pthread_mutex_destroy(&app.state_mtx);
        return 1;
    }

    if (media_arg) {
        load_media(&app, media_arg);
    }

    gtk_main();

    g_atomic_int_set(&app.running, 0);
    pthread_join(app.event_thread, NULL);

    if (app.hide_timeout_id > 0) {
        g_source_remove(app.hide_timeout_id);
    }
    if (app.idle_source_id > 0) {
        g_source_remove(app.idle_source_id);
    }
    if (app.mpv_ctx) {
        mpv_render_context_set_update_callback(app.mpv_ctx, NULL, NULL);
        mpv_render_context_free(app.mpv_ctx);
    }
    destroy_egl_gl(&app);
    if (app.mpv) {
        mpv_terminate_destroy(app.mpv);
    }

    pthread_mutex_lock(&app.frame_mtx);
    destroy_frame_buffer_locked(&app);
    pthread_mutex_unlock(&app.frame_mtx);

    pthread_mutex_destroy(&app.frame_mtx);
    pthread_mutex_destroy(&app.state_mtx);
    return 0;
}
