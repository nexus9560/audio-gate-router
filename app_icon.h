#ifndef APP_ICON_H
#define APP_ICON_H

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <stdint.h>
#include <stdbool.h>
#include "app_icon_data.h"

#define APP_ICON_SIZE 64
#define APP_ICON_NAME "org.fedora.audiogaterouter"

/* Decodes the baked-in app_icon.png bytes into a GdkPixbuf scaled to size x size */
static inline GdkPixbuf *load_baked_png_pixbuf(int size) {
    GInputStream *stream = g_memory_input_stream_new_from_data(
        app_icon_png, app_icon_png_len, NULL
    );
    GdkPixbuf *raw = gdk_pixbuf_new_from_stream(stream, NULL, NULL);
    g_object_unref(stream);
    if (!raw) return NULL;

    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(raw, size, size, GDK_INTERP_BILINEAR);
    g_object_unref(raw);
    return scaled;
}

/* Builds the D-Bus Variant 'a(iiay)' (Network Byte Order ARGB32) for KDE StatusNotifierItem */
static inline GVariant *build_sni_icon_pixmap_variant(void) {
    GdkPixbuf *pb = load_baked_png_pixbuf(APP_ICON_SIZE);
    if (!pb) {
        GVariantBuilder empty;
        g_variant_builder_init(&empty, G_VARIANT_TYPE("a(iiay)"));
        return g_variant_builder_end(&empty);
    }

    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int rowstride = gdk_pixbuf_get_rowstride(pb);
    int n_channels = gdk_pixbuf_get_n_channels(pb);
    const guchar *pixels = gdk_pixbuf_read_pixels(pb);

    gsize byte_len = (gsize)w * h * 4;
    uint8_t *argb = g_malloc(byte_len);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const guchar *p = pixels + y * rowstride + x * n_channels;
            uint8_t r = p[0];
            uint8_t g = p[1];
            uint8_t b = p[2];
            uint8_t a = (n_channels >= 4) ? p[3] : 255;

            // SNI spec requires straight Big-Endian ARGB32: [A, R, G, B]
            int idx = (y * w + x) * 4;
            argb[idx + 0] = a;
            argb[idx + 1] = r;
            argb[idx + 2] = g;
            argb[idx + 3] = b;
        }
    }
    g_object_unref(pb);

    GVariant *byte_array = g_variant_new_fixed_array(
        G_VARIANT_TYPE_BYTE, argb, byte_len, sizeof(uint8_t)
    );
    g_free(argb);

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
    g_variant_builder_add(&builder, "(ii@ay)", w, h, byte_array);
    return g_variant_builder_end(&builder);
}

/* Returns the custom icon directory where the baked PNG is written */
static inline char *get_baked_icon_theme_dir(void) {
    return g_build_filename(g_get_user_data_dir(), "audio_gate_router", "icons", NULL);
}

/* Exports the baked-in PNG so both KDE SNI (IconThemePath + IconPixmap) and GTK4 find it */
static inline void register_baked_app_icon(void) {
    char *sni_icon_dir = get_baked_icon_theme_dir();
    g_mkdir_with_parents(sni_icon_dir, 0755);

    // Write directly into sni_icon_dir for KDE's IconThemePath lookup
    char *sni_png_path = g_build_filename(sni_icon_dir, APP_ICON_NAME ".png", NULL);
    g_file_set_contents(sni_png_path, (const gchar *)app_icon_png, app_icon_png_len, NULL);

    // Also write into ~/.local/share/icons/hicolor/512x512/apps for GTK4 & .desktop launcher
    const char *data_dir = g_get_user_data_dir();
    char *hicolor_dir = g_build_filename(data_dir, "icons", "hicolor", "512x512", "apps", NULL);
    g_mkdir_with_parents(hicolor_dir, 0755);

    char *hicolor_path = g_build_filename(hicolor_dir, APP_ICON_NAME ".png", NULL);
    g_file_set_contents(hicolor_path, (const gchar *)app_icon_png, app_icon_png_len, NULL);

    char *theme_base = g_build_filename(data_dir, "icons", NULL);
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    gtk_icon_theme_add_search_path(theme, theme_base);
    gtk_icon_theme_add_search_path(theme, sni_icon_dir);

    gtk_window_set_default_icon_name(APP_ICON_NAME);

    g_free(theme_base);
    g_free(hicolor_path);
    g_free(hicolor_dir);
    g_free(sni_png_path);
    g_free(sni_icon_dir);
}

#endif /* APP_ICON_H */
