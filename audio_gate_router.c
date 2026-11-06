#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_icon.h"

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define CHUNK_FRAMES 256   // ~5.3ms chunks for instant gate detection
#define HOLD_TIME_MS 600   // Hold direct link open for 600ms after noise stops
#define UI_UPDATE_INTERVAL_MS 100 // Throttle UI label redraws to 10Hz max
#define CONFIG_SYNC_INTERVAL_SEC 3600 // Diff and sync every 1 hour (3600s)

typedef struct {
    char *name;
    char *description;
} AudioDevice;

typedef struct {
    GPtrArray *sources;
    GPtrArray *sinks;
} DeviceList;

struct RouteEntry;

/* Atomically reference-counted context shared safely between GTK & worker thread */
typedef struct {
    struct RouteEntry *owner_entry; // Accessed only on GTK main thread
    volatile gint running;
    volatile gint active_gate;
    volatile gint ui_update_pending;
    volatile double current_db;
    volatile double threshold_db;

    char *source_name;
    char *source_port;
    char *sink_name;
    char *sink_port;
} WorkerContext;

typedef struct RouteEntry {
    GtkWidget *row_box;
    GtkWidget *input_dd;
    GtkWidget *in_port_dd;
    GtkWidget *output_dd;
    GtkWidget *out_port_dd;
    GtkWidget *thresh_spin;
    GtkWidget *enable_switch;
    GtkWidget *status_label;

    GPtrArray *in_ports;
    GPtrArray *out_ports;

    WorkerContext *active_worker;
} RouteEntry;

static GtkWidget *main_window = NULL;
static GtkWidget *list_box = NULL;
static GtkWidget *launch_login_switch = NULL;
static GtkWidget *start_min_switch = NULL;
static GApplication *global_app = NULL;
static DeviceList dev_list = {NULL, NULL};
static GPtrArray *route_entries = NULL;

static bool opt_launch_on_login = false;
static bool opt_start_minimized = false;
static bool shutting_down = false;

/* Forward Declarations */
static RouteEntry *add_route_row(const char *src_node, const char *src_port,
                                 const char *snk_node, const char *snk_port,
                                 double threshold, bool active);
static void sync_config_if_diff(bool force_initial);
static void update_autostart_desktop_file(bool enable);
static void perform_clean_shutdown(void);

/* =========================================================================
 * KDE / FreeDesktop StatusNotifierItem & DBusMenu System Tray Implementation
 * ========================================================================= */
static const gchar sni_introspection_xml[] =
    "<node>"
    "  <interface name='org.kde.StatusNotifierItem'>"
    "    <property name='Category' type='s' access='read'/>"
    "    <property name='Id' type='s' access='read'/>"
    "    <property name='Title' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='WindowId' type='i' access='read'/>"
    "    <property name='IconThemePath' type='s' access='read'/>"
    "    <property name='IconName' type='s' access='read'/>"
    "    <property name='IconPixmap' type='a(iiay)' access='read'/>"
    "    <property name='OverlayIconName' type='s' access='read'/>"
    "    <property name='OverlayIconPixmap' type='a(iiay)' access='read'/>"
    "    <property name='AttentionIconName' type='s' access='read'/>"
    "    <property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"
    "    <property name='ItemIsMenu' type='b' access='read'/>"
    "    <property name='Menu' type='o' access='read'/>"
    "    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "    <method name='Activate'>"
    "      <arg name='x' type='i' direction='in'/>"
    "      <arg name='y' type='i' direction='in'/>"
    "    </method>"
    "    <method name='ContextMenu'>"
    "      <arg name='x' type='i' direction='in'/>"
    "      <arg name='y' type='i' direction='in'/>"
    "    </method>"
    "    <method name='SecondaryActivate'>"
    "      <arg name='x' type='i' direction='in'/>"
    "      <arg name='y' type='i' direction='in'/>"
    "    </method>"
    "    <signal name='NewIcon'/>"
    "    <signal name='NewToolTip'/>"
    "    <signal name='NewStatus'>"
    "      <arg name='status' type='s'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

static const gchar dbusmenu_introspection_xml[] =
    "<node>"
    "  <interface name='com.canonical.dbusmenu'>"
    "    <property name='Version' type='u' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <method name='GetLayout'>"
    "      <arg type='i' name='parentId' direction='in'/>"
    "      <arg type='i' name='recursionDepth' direction='in'/>"
    "      <arg type='as' name='propertyNames' direction='in'/>"
    "      <arg type='u' name='revision' direction='out'/>"
    "      <arg type='(ia{sv}av)' name='layout' direction='out'/>"
    "    </method>"
    "    <method name='GetGroupProperties'>"
    "      <arg type='ai' name='ids' direction='in'/>"
    "      <arg type='as' name='propertyNames' direction='in'/>"
    "      <arg type='a(ia{sv})' name='properties' direction='out'/>"
    "    </method>"
    "    <method name='Event'>"
    "      <arg type='i' name='id' direction='in'/>"
    "      <arg type='s' name='eventId' direction='in'/>"
    "      <arg type='v' name='data' direction='in'/>"
    "      <arg type='u' name='timestamp' direction='in'/>"
    "    </method>"
    "    <method name='AboutToShow'>"
    "      <arg type='i' name='id' direction='in'/>"
    "      <arg type='b' name='needUpdate' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static void toggle_main_window(void) {
    if (!main_window) return;
    if (gtk_widget_get_visible(main_window)) {
        gtk_widget_set_visible(main_window, FALSE);
    } else {
        gtk_window_present(GTK_WINDOW(main_window));
    }
}

static void sni_method_call(GDBusConnection *conn, const gchar *sender, const gchar *path,
                            const gchar *interface, const gchar *method, GVariant *params,
                            GDBusMethodInvocation *invocation, gpointer user_data) {
    if (g_strcmp0(method, "Activate") == 0 || g_strcmp0(method, "SecondaryActivate") == 0 ||
        g_strcmp0(method, "ContextMenu") == 0) {
        toggle_main_window();
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}

static GVariant *sni_get_property(GDBusConnection *conn, const gchar *sender, const gchar *path,
                                  const gchar *interface, const gchar *prop, GError **error,
                                  gpointer user_data) {
    if (g_strcmp0(prop, "Category") == 0) return g_variant_new_string("ApplicationStatus");
    if (g_strcmp0(prop, "Id") == 0) return g_variant_new_string("audio_gate_router");
    if (g_strcmp0(prop, "Title") == 0) return g_variant_new_string("PipeWire Audio Gate Patchbay");
    if (g_strcmp0(prop, "Status") == 0) return g_variant_new_string("Active");
    if (g_strcmp0(prop, "WindowId") == 0) return g_variant_new_int32(0);
    if (g_strcmp0(prop, "IconThemePath") == 0) {
        char *dir = get_baked_icon_theme_dir();
        GVariant *v = g_variant_new_string(dir);
        g_free(dir);
        return v;
    }
    if (g_strcmp0(prop, "IconName") == 0) return g_variant_new_string("");
    if (g_strcmp0(prop, "IconPixmap") == 0) return build_sni_icon_pixmap_variant();
    if (g_strcmp0(prop, "OverlayIconName") == 0) return g_variant_new_string("");
    if (g_strcmp0(prop, "AttentionIconName") == 0) return g_variant_new_string("");
    if (g_strcmp0(prop, "OverlayIconPixmap") == 0 || g_strcmp0(prop, "AttentionIconPixmap") == 0) {
        GVariantBuilder empty;
        g_variant_builder_init(&empty, G_VARIANT_TYPE("a(iiay)"));
        return g_variant_builder_end(&empty);
    }
    if (g_strcmp0(prop, "ItemIsMenu") == 0) return g_variant_new_boolean(FALSE);
    if (g_strcmp0(prop, "Menu") == 0) return g_variant_new_object_path("/MenuBar");
    if (g_strcmp0(prop, "ToolTip") == 0) {
        GVariant *pixmaps = build_sni_icon_pixmap_variant();
        return g_variant_new("(s@a(iiay)ss)", "", pixmaps,
                             "PipeWire Audio Gate Router",
                             "Left-click to show/hide window, Right-click for menu");
    }
    return NULL;
}

static const GDBusInterfaceVTable sni_vtable = {
    sni_method_call,
    sni_get_property,
    NULL,
    { 0 }
};

static GVariant *build_menu_item(gint id, const gchar *label) {
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "label", g_variant_new_string(label));
    g_variant_builder_add(&props, "{sv}", "enabled", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));

    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    return g_variant_new("(ia{sv}av)", id, &props, &children);
}

static void menu_method_call(GDBusConnection *conn, const gchar *sender, const gchar *path,
                             const gchar *interface, const gchar *method, GVariant *params,
                             GDBusMethodInvocation *invocation, gpointer user_data) {
    if (g_strcmp0(method, "GetLayout") == 0) {
        GVariantBuilder root_props;
        g_variant_builder_init(&root_props, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&root_props, "{sv}", "children-display", g_variant_new_string("submenu"));

        GVariantBuilder children;
        g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
        g_variant_builder_add(&children, "v", build_menu_item(1, "Show / Hide Patchbay"));
        g_variant_builder_add(&children, "v", build_menu_item(2, "Quit Completely"));

        GVariant *root = g_variant_new("(ia{sv}av)", 0, &root_props, &children);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@(ia{sv}av))", 1, root));
    } else if (g_strcmp0(method, "GetGroupProperties") == 0) {
        GVariantBuilder res;
        g_variant_builder_init(&res, G_VARIANT_TYPE("a(ia{sv})"));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ia{sv}))", &res));
    } else if (g_strcmp0(method, "AboutToShow") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
    } else if (g_strcmp0(method, "Event") == 0) {
        gint id;
        const gchar *event_id;
        GVariant *data;
        guint timestamp;
        g_variant_get(params, "(isvu)", &id, &event_id, &data, &timestamp);
        if (g_strcmp0(event_id, "clicked") == 0) {
            if (id == 1) {
                toggle_main_window();
            } else if (id == 2 && global_app) {
                perform_clean_shutdown();
                g_application_quit(global_app);
            }
        }
        g_variant_unref(data);
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}

static GVariant *menu_get_property(GDBusConnection *conn, const gchar *sender, const gchar *path,
                                   const gchar *interface, const gchar *prop, GError **error,
                                   gpointer user_data) {
    if (g_strcmp0(prop, "Version") == 0) return g_variant_new_uint32(3);
    if (g_strcmp0(prop, "Status") == 0) return g_variant_new_string("normal");
    return NULL;
}

static const GDBusInterfaceVTable menu_vtable = {
    menu_method_call,
    menu_get_property,
    NULL,
    { 0 }
};

static void register_with_watcher(GDBusConnection *conn, const gchar *bus_name) {
    g_dbus_connection_call(
        conn,
        "org.kde.StatusNotifierWatcher",
        "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher",
        "RegisterStatusNotifierItem",
        g_variant_new("(s)", bus_name),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL
    );
}

static void on_sni_name_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data) {
    register_with_watcher(conn, name);
}

static void setup_system_tray(void) {
    GError *error = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!conn) return;

    GDBusNodeInfo *sni_info = g_dbus_node_info_new_for_xml(sni_introspection_xml, NULL);
    GDBusNodeInfo *menu_info = g_dbus_node_info_new_for_xml(dbusmenu_introspection_xml, NULL);

    g_dbus_connection_register_object(conn, "/StatusNotifierItem", sni_info->interfaces[0],
                                      &sni_vtable, NULL, NULL, NULL);
    g_dbus_connection_register_object(conn, "/MenuBar", menu_info->interfaces[0],
                                      &menu_vtable, NULL, NULL, NULL);

    char bus_name[128];
    snprintf(bus_name, sizeof(bus_name), "org.kde.StatusNotifierItem-%d-1", getpid());
    g_bus_own_name_on_connection(conn, bus_name, G_BUS_NAME_OWNER_FLAGS_NONE,
                                 on_sni_name_acquired, NULL, NULL, NULL);

    g_dbus_node_info_unref(sni_info);
    g_dbus_node_info_unref(menu_info);
}

/* =========================================================================
 * XDG Autostart Management & Quiet Configuration File Sync
 * ========================================================================= */
static void update_autostart_desktop_file(bool enable) {
    const char *cfg_dir = g_get_user_config_dir();
    char *autostart_dir = g_build_filename(cfg_dir, "autostart", NULL);
    char *desktop_path = g_build_filename(autostart_dir, "audio_gate_router.desktop", NULL);

    if (enable) {
        g_mkdir_with_parents(autostart_dir, 0700);
        char exe_path[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len != -1) {
            exe_path[len] = '\0';
        } else {
            snprintf(exe_path, sizeof(exe_path), "audio_gate_router");
        }

        char content[PATH_MAX + 512];
        snprintf(content, sizeof(content),
                 "[Desktop Entry]\n"
                 "Type=Application\n"
                 "Name=PipeWire Audio Gate Router\n"
                 "Comment=Noise-Gated PipeWire Direct Patchbay\n"
                 "Exec=\"%s\"\n"
                 "Icon=" APP_ICON_NAME "\n"
                 "StartupWMClass=" APP_ICON_NAME "\n"
                 "Terminal=false\n"
                 "Categories=AudioVideo;Audio;\n"
                 "X-GNOME-Autostart-enabled=true\n",
                 exe_path);

        g_file_set_contents(desktop_path, content, -1, NULL);
    } else {
        unlink(desktop_path);
    }

    g_free(desktop_path);
    g_free(autostart_dir);
}

static char *get_config_file_path(void) {
    const char *cfg_dir = g_get_user_config_dir();
    char *app_dir = g_build_filename(cfg_dir, "audio_gate_router", NULL);
    g_mkdir_with_parents(app_dir, 0700);
    char *file_path = g_build_filename(app_dir, "routes.conf", NULL);
    g_free(app_dir);
    return file_path;
}

static char *serialize_current_config(gsize *out_len) {
    GKeyFile *kf = g_key_file_new();
    guint valid_count = 0;

    bool launch_login = launch_login_switch
                        ? gtk_switch_get_active(GTK_SWITCH(launch_login_switch))
                        : opt_launch_on_login;
    bool start_min = start_min_switch
                     ? gtk_switch_get_active(GTK_SWITCH(start_min_switch))
                     : opt_start_minimized;

    g_key_file_set_boolean(kf, "General", "launch_on_login", launch_login);
    g_key_file_set_boolean(kf, "General", "start_minimized", start_min);

    if (route_entries) {
        for (guint i = 0; i < route_entries->len; i++) {
            RouteEntry *entry = g_ptr_array_index(route_entries, i);

            guint in_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(entry->input_dd));
            guint in_p_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(entry->in_port_dd));
            guint out_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(entry->output_dd));
            guint out_p_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(entry->out_port_dd));

            if (in_idx == GTK_INVALID_LIST_POSITION || out_idx == GTK_INVALID_LIST_POSITION ||
                in_idx >= dev_list.sources->len || out_idx >= dev_list.sinks->len) {
                continue;
            }

            AudioDevice *src = g_ptr_array_index(dev_list.sources, in_idx);
            AudioDevice *snk = g_ptr_array_index(dev_list.sinks, out_idx);

            const char *src_port = (entry->in_ports && in_p_idx < entry->in_ports->len)
                                   ? g_ptr_array_index(entry->in_ports, in_p_idx)
                                   : "Stereo (FL + FR)";
            const char *snk_port = (entry->out_ports && out_p_idx < entry->out_ports->len)
                                   ? g_ptr_array_index(entry->out_ports, out_p_idx)
                                   : "Stereo (FL + FR)";

            double thresh = gtk_spin_button_get_value(GTK_SPIN_BUTTON(entry->thresh_spin));
            bool active = gtk_switch_get_active(GTK_SWITCH(entry->enable_switch));

            char group[64];
            snprintf(group, sizeof(group), "Route_%u", valid_count++);

            g_key_file_set_string(kf, group, "source_node", src->name);
            g_key_file_set_string(kf, group, "source_port", src_port);
            g_key_file_set_string(kf, group, "sink_node", snk->name);
            g_key_file_set_string(kf, group, "sink_port", snk_port);
            g_key_file_set_double(kf, group, "threshold_db", thresh);
            g_key_file_set_boolean(kf, group, "active", active);
        }
    }

    g_key_file_set_integer(kf, "General", "route_count", valid_count);
    char *data = g_key_file_to_data(kf, out_len, NULL);
    g_key_file_free(kf);
    return data;
}

static void sync_config_if_diff(bool force_initial) {
    char *path = get_config_file_path();
    gsize new_len = 0;
    char *new_data = serialize_current_config(&new_len);

    char *old_data = NULL;
    gsize old_len = 0;
    bool file_exists = g_file_get_contents(path, &old_data, &old_len, NULL);

    if (force_initial || !file_exists || g_strcmp0(old_data, new_data) != 0) {
        g_file_set_contents(path, new_data, new_len, NULL);
    }

    g_free(old_data);
    g_free(new_data);
    g_free(path);
}

static gboolean hourly_config_sync_cb(gpointer user_data) {
    sync_config_if_diff(false);
    return G_SOURCE_CONTINUE;
}

static bool load_config_quietly(void) {
    char *path = get_config_file_path();
    GKeyFile *kf = g_key_file_new();

    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        opt_launch_on_login = false;
        opt_start_minimized = false;
        g_key_file_free(kf);
        g_free(path);
        return false;
    }

    GError *err = NULL;
    bool l_login = g_key_file_get_boolean(kf, "General", "launch_on_login", &err);
    if (err) {
        opt_launch_on_login = false;
        g_clear_error(&err);
    } else {
        opt_launch_on_login = l_login;
    }

    bool s_min = g_key_file_get_boolean(kf, "General", "start_minimized", &err);
    if (err) {
        opt_start_minimized = false;
        g_clear_error(&err);
    } else {
        opt_start_minimized = s_min;
    }

    gtk_switch_set_active(GTK_SWITCH(launch_login_switch), opt_launch_on_login);
    gtk_switch_set_active(GTK_SWITCH(start_min_switch), opt_start_minimized);
    update_autostart_desktop_file(opt_launch_on_login);

    int count = g_key_file_get_integer(kf, "General", "route_count", NULL);
    int loaded = 0;

    for (int i = 0; i < count; i++) {
        char group[64];
        snprintf(group, sizeof(group), "Route_%d", i);
        if (!g_key_file_has_group(kf, group)) continue;

        char *src_node = g_key_file_get_string(kf, group, "source_node", NULL);
        char *src_port = g_key_file_get_string(kf, group, "source_port", NULL);
        char *snk_node = g_key_file_get_string(kf, group, "sink_node", NULL);
        char *snk_port = g_key_file_get_string(kf, group, "sink_port", NULL);
        double thresh = g_key_file_get_double(kf, group, "threshold_db", NULL);
        bool active = g_key_file_get_boolean(kf, group, "active", NULL);

        add_route_row(src_node, src_port, snk_node, snk_port, thresh, active);
        loaded++;

        g_free(src_node);
        g_free(src_port);
        g_free(snk_node);
        g_free(snk_port);
    }

    g_key_file_free(kf);
    g_free(path);
    return (loaded > 0);
}

/* =========================================================================
 * Audio Devices & Direct PipeWire Graph Routing
 * ========================================================================= */
static void free_device(gpointer data) {
    AudioDevice *dev = (AudioDevice *)data;
    if (dev) {
        g_free(dev->name);
        g_free(dev->description);
        g_free(dev);
    }
}

static GPtrArray *get_ports_for_node(const char *node_name, bool is_input_node) {
    GPtrArray *ports = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(ports, g_strdup("Stereo (FL + FR)"));

    if (!node_name) return ports;

    char *argv[] = {"pw-link", is_input_node ? "-o" : "-i", NULL};
    char *stdout_buf = NULL;

    if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, &stdout_buf, NULL, NULL, NULL) && stdout_buf) {
        char prefix[512];
        snprintf(prefix, sizeof(prefix), "%s:", node_name);
        size_t prefix_len = strlen(prefix);

        char **lines = g_strsplit(stdout_buf, "\n", -1);
        for (int i = 0; lines[i] != NULL; i++) {
            char *line = g_strstrip(lines[i]);
            if (g_str_has_prefix(line, prefix)) {
                const char *port_short = line + prefix_len;
                if (strlen(port_short) > 0) {
                    g_ptr_array_add(ports, g_strdup(port_short));
                }
            }
        }
        g_strfreev(lines);
        g_free(stdout_buf);
    }

    if (ports->len == 1) {
        if (is_input_node) {
            g_ptr_array_add(ports, g_strdup("capture_FL"));
            g_ptr_array_add(ports, g_strdup("capture_FR"));
            g_ptr_array_add(ports, g_strdup("capture_MONO"));
        } else {
            g_ptr_array_add(ports, g_strdup("playback_FL"));
            g_ptr_array_add(ports, g_strdup("playback_FR"));
            g_ptr_array_add(ports, g_strdup("playback_FC"));
            g_ptr_array_add(ports, g_strdup("playback_LFE"));
            g_ptr_array_add(ports, g_strdup("playback_RL"));
            g_ptr_array_add(ports, g_strdup("playback_RR"));
        }
    }
    return ports;
}

static void populate_port_dropdown(GtkWidget *dd, GPtrArray *ports) {
    const char **strings = g_malloc0((ports->len + 1) * sizeof(char *));
    for (guint i = 0; i < ports->len; i++) {
        strings[i] = (const char *)g_ptr_array_index(ports, i);
    }
    strings[ports->len] = NULL;
    GtkStringList *list = gtk_string_list_new(strings);
    gtk_drop_down_set_model(GTK_DROP_DOWN(dd), G_LIST_MODEL(list));
    g_object_unref(list);
    g_free(strings);
}

static void link_port_pair(const char *out_port, const char *in_port, bool connect) {
    char *argv[] = {
        "pw-link",
        connect ? (char *)out_port : "-d",
        connect ? (char *)in_port  : (char *)out_port,
        connect ? NULL             : (char *)in_port,
        NULL
    };
    g_spawn_sync(NULL, argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, NULL, NULL, NULL, NULL);
}

static void set_pipewire_direct_link(WorkerContext *wctx, bool connect) {
    if (!wctx || !wctx->source_name || !wctx->sink_name || !wctx->source_port || !wctx->sink_port) {
        return;
    }

    bool src_stereo = (g_strcmp0(wctx->source_port, "Stereo (FL + FR)") == 0);
    bool snk_stereo = (g_strcmp0(wctx->sink_port, "Stereo (FL + FR)") == 0);

    char src_fl[512], src_fr[512], src_mono[512], src_single[512];
    char snk_fl[512], snk_fr[512], snk_single[512];

    snprintf(src_fl, sizeof(src_fl), "%s:capture_FL", wctx->source_name);
    snprintf(src_fr, sizeof(src_fr), "%s:capture_FR", wctx->source_name);
    snprintf(src_mono, sizeof(src_mono), "%s:capture_MONO", wctx->source_name);
    snprintf(src_single, sizeof(src_single), "%s:%s", wctx->source_name, wctx->source_port);

    snprintf(snk_fl, sizeof(snk_fl), "%s:playback_FL", wctx->sink_name);
    snprintf(snk_fr, sizeof(snk_fr), "%s:playback_FR", wctx->sink_name);
    snprintf(snk_single, sizeof(snk_single), "%s:%s", wctx->sink_name, wctx->sink_port);

    if (src_stereo && snk_stereo) {
        link_port_pair(src_fl, snk_fl, connect);
        link_port_pair(src_fr, snk_fr, connect);
        link_port_pair(src_mono, snk_fl, connect);
        link_port_pair(src_mono, snk_fr, connect);
    } else if (src_stereo && !snk_stereo) {
        link_port_pair(src_fl, snk_single, connect);
        link_port_pair(src_fr, snk_single, connect);
        link_port_pair(src_mono, snk_single, connect);
    } else if (!src_stereo && snk_stereo) {
        link_port_pair(src_single, snk_fl, connect);
        link_port_pair(src_single, snk_fr, connect);
    } else {
        link_port_pair(src_single, snk_single, connect);
    }
}

static void source_info_cb(pa_context *c, const pa_source_info *i, int eol, void *userdata) {
    if (eol > 0 || !i) return;
    DeviceList *dl = (DeviceList *)userdata;
    AudioDevice *dev = g_malloc0(sizeof(AudioDevice));
    dev->name = g_strdup(i->name);
    dev->description = g_strdup(i->description ? i->description : i->name);
    g_ptr_array_add(dl->sources, dev);
}

static void sink_info_cb(pa_context *c, const pa_sink_info *i, int eol, void *userdata) {
    if (eol > 0 || !i) return;
    DeviceList *dl = (DeviceList *)userdata;
    AudioDevice *dev = g_malloc0(sizeof(AudioDevice));
    dev->name = g_strdup(i->name);
    dev->description = g_strdup(i->description ? i->description : i->name);
    g_ptr_array_add(dl->sinks, dev);
}

static void refresh_devices(DeviceList *dl) {
    if (dl->sources) g_ptr_array_free(dl->sources, TRUE);
    if (dl->sinks) g_ptr_array_free(dl->sinks, TRUE);
    dl->sources = g_ptr_array_new_with_free_func(free_device);
    dl->sinks = g_ptr_array_new_with_free_func(free_device);

    pa_mainloop *ml = pa_mainloop_new();
    pa_mainloop_api *mlapi = pa_mainloop_get_api(ml);
    pa_context *ctx = pa_context_new(mlapi, "AudioGateEnumerator");

    pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);

    while (1) {
        pa_context_state_t state = pa_context_get_state(ctx);
        if (state == PA_CONTEXT_READY) break;
        if (!PA_CONTEXT_IS_GOOD(state)) goto cleanup;
        pa_mainloop_iterate(ml, 1, NULL);
    }

    pa_operation *op1 = pa_context_get_source_info_list(ctx, source_info_cb, dl);
    while (pa_operation_get_state(op1) == PA_OPERATION_RUNNING) {
        pa_mainloop_iterate(ml, 1, NULL);
    }
    pa_operation_unref(op1);

    pa_operation *op2 = pa_context_get_sink_info_list(ctx, sink_info_cb, dl);
    while (pa_operation_get_state(op2) == PA_OPERATION_RUNNING) {
        pa_mainloop_iterate(ml, 1, NULL);
    }
    pa_operation_unref(op2);

cleanup:
    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);
}

/* Destructor called by g_atomic_rc_box_release_full ONLY when refcount hits 0 */
static void worker_context_clear(gpointer data) {
    WorkerContext *wctx = (WorkerContext *)data;
    g_free(wctx->source_name);
    g_free(wctx->source_port);
    g_free(wctx->sink_name);
    g_free(wctx->sink_port);
}

static void worker_context_unref(gpointer data) {
    if (data) {
        g_atomic_rc_box_release_full(data, worker_context_clear);
    }
}

/* Safe, throttled UI callback on GTK Main Thread */
static gboolean update_row_ui(gpointer user_data) {
    WorkerContext *wctx = (WorkerContext *)user_data;
    g_atomic_int_set(&wctx->ui_update_pending, 0);

    if (shutting_down) return G_SOURCE_REMOVE;

    RouteEntry *entry = wctx->owner_entry;
    if (!entry || entry->active_worker != wctx) {
        return G_SOURCE_REMOVE;
    }

    if (!g_atomic_int_get(&wctx->running)) {
        gtk_label_set_markup(GTK_LABEL(entry->status_label), "<span foreground='gray'>Stopped</span>");
        return G_SOURCE_REMOVE;
    }

    if (main_window && !gtk_widget_get_visible(main_window)) {
        return G_SOURCE_REMOVE;
    }

    char buf[128];
    if (g_atomic_int_get(&wctx->active_gate)) {
        snprintf(buf, sizeof(buf), "<span foreground='#2ec27e' weight='bold'>LINKED (%.1f dB)</span>", wctx->current_db);
    } else {
        snprintf(buf, sizeof(buf), "<span foreground='#e01b24'>Unlinked (%.1f dB)</span>", wctx->current_db);
    }
    gtk_label_set_markup(GTK_LABEL(entry->status_label), buf);
    return G_SOURCE_REMOVE;
}

static void schedule_ui_update(WorkerContext *wctx) {
    if (g_atomic_int_compare_and_exchange(&wctx->ui_update_pending, 0, 1)) {
        // Acquire a reference for the idle callback; automatically released via worker_context_unref
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                        update_row_ui,
                        g_atomic_rc_box_acquire(wctx),
                        worker_context_unref);
    }
}

/* Detached worker thread with atomic reference counting */
static void *route_worker(void *arg) {
    WorkerContext *wctx = (WorkerContext *)arg;
    static const pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = SAMPLE_RATE,
        .channels = CHANNELS
    };

    pa_buffer_attr buf_attr = {
        .maxlength = (uint32_t)-1,
        .tlength   = (uint32_t)-1,
        .prebuf    = (uint32_t)-1,
        .minreq    = (uint32_t)-1,
        .fragsize  = CHUNK_FRAMES * CHANNELS * sizeof(int16_t)
    };

    int16_t buffer[CHUNK_FRAMES * CHANNELS];
    gint64 last_active_time = 0;
    gint64 last_ui_time = 0;
    bool is_linked = false;

    set_pipewire_direct_link(wctx, false);

    while (g_atomic_int_get(&wctx->running)) {
        int error = 0;
        pa_simple *in_stream = pa_simple_new(
            NULL, "AudioGateDetector", PA_STREAM_RECORD, wctx->source_name,
            "Level Trigger", &ss, NULL, &buf_attr, &error
        );

        if (!in_stream) {
            for (int i = 0; i < 10 && g_atomic_int_get(&wctx->running); i++) {
                g_usleep(100000);
            }
            continue;
        }

        while (g_atomic_int_get(&wctx->running)) {
            if (pa_simple_read(in_stream, buffer, sizeof(buffer), &error) < 0) {
                break;
            }

            if (!g_atomic_int_get(&wctx->running)) break;

            double sum = 0.0;
            int total_samples = CHUNK_FRAMES * CHANNELS;
            for (int i = 0; i < total_samples; i++) {
                double norm = buffer[i] / 32768.0;
                sum += norm * norm;
            }
            double rms = sqrt(sum / total_samples);
            double db = (rms > 0.000001) ? 20.0 * log10(rms) : -90.0;
            wctx->current_db = db;

            gint64 now = g_get_monotonic_time() / 1000;
            if (db >= wctx->threshold_db) {
                last_active_time = now;
            }

            bool should_wire = (now - last_active_time) <= HOLD_TIME_MS;
            bool state_changed = false;

            if (should_wire && !is_linked) {
                set_pipewire_direct_link(wctx, true);
                is_linked = true;
                g_atomic_int_set(&wctx->active_gate, 1);
                state_changed = true;
            } else if (!should_wire && is_linked) {
                set_pipewire_direct_link(wctx, false);
                is_linked = false;
                g_atomic_int_set(&wctx->active_gate, 0);
                state_changed = true;
            }

            if (state_changed || (now - last_ui_time) >= UI_UPDATE_INTERVAL_MS) {
                last_ui_time = now;
                schedule_ui_update(wctx);
            }
        }

        pa_simple_free(in_stream);
    }

    set_pipewire_direct_link(wctx, false);
    // Release the worker thread's own reference to wctx
    worker_context_unref(wctx);
    return NULL;
}

/* --- GUI Callbacks & Non-Blocking Cleanup Helpers --- */
static void stop_entry(RouteEntry *entry) {
    if (!entry || !entry->active_worker) return;

    WorkerContext *wctx = entry->active_worker;
    entry->active_worker = NULL;

    wctx->owner_entry = NULL;
    g_atomic_int_set(&wctx->running, 0);

    set_pipewire_direct_link(wctx, false);

    if (!shutting_down && entry->status_label) {
        gtk_label_set_markup(GTK_LABEL(entry->status_label), "<span foreground='gray'>Stopped</span>");
    }

    // Release the GUI row's reference to wctx
    worker_context_unref(wctx);
}

static void perform_clean_shutdown(void) {
    if (shutting_down) return;
    shutting_down = true;

    sync_config_if_diff(false);

    if (route_entries) {
        for (guint i = 0; i < route_entries->len; i++) {
            RouteEntry *entry = g_ptr_array_index(route_entries, i);
            stop_entry(entry);
        }
    }
}

static gboolean on_unix_signal_quit(gpointer user_data) {
    perform_clean_shutdown();
    if (global_app) {
        g_application_quit(global_app);
    }
    return G_SOURCE_REMOVE;
}

static void on_app_shutdown(GApplication *app, gpointer user_data) {
    perform_clean_shutdown();
}

static gboolean on_launch_login_state_set(GtkSwitch *sw, gboolean state, gpointer user_data) {
    opt_launch_on_login = state;
    update_autostart_desktop_file(state);
    return FALSE;
}

static gboolean on_start_min_state_set(GtkSwitch *sw, gboolean state, gpointer user_data) {
    opt_start_minimized = state;
    return FALSE;
}

static void on_input_node_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    RouteEntry *entry = (RouteEntry *)user_data;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(entry->input_dd));
    if (idx == GTK_INVALID_LIST_POSITION || idx >= dev_list.sources->len) return;

    AudioDevice *src = g_ptr_array_index(dev_list.sources, idx);
    if (entry->in_ports) g_ptr_array_free(entry->in_ports, TRUE);
    entry->in_ports = get_ports_for_node(src->name, true);
    populate_port_dropdown(entry->in_port_dd, entry->in_ports);
}

static void on_output_node_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    RouteEntry *entry = (RouteEntry *)user_data;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(entry->output_dd));
    if (idx == GTK_INVALID_LIST_POSITION || idx >= dev_list.sinks->len) return;

    AudioDevice *snk = g_ptr_array_index(dev_list.sinks, idx);
    if (entry->out_ports) g_ptr_array_free(entry->out_ports, TRUE);
    entry->out_ports = get_ports_for_node(snk->name, false);
    populate_port_dropdown(entry->out_port_dd, entry->out_ports);
}

static void on_thresh_changed(GtkSpinButton *spin, gpointer user_data) {
    RouteEntry *entry = (RouteEntry *)user_data;
    if (entry->active_worker) {
        entry->active_worker->threshold_db = gtk_spin_button_get_value(spin);
    }
}

static gboolean on_switch_state_set(GtkSwitch *sw, gboolean state, gpointer user_data) {
    RouteEntry *entry = (RouteEntry *)user_data;

    if (state) {
        stop_entry(entry);

        guint in_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(entry->input_dd));
        guint in_p_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(entry->in_port_dd));
        guint out_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(entry->output_dd));
        guint out_p_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(entry->out_port_dd));

        if (in_idx == GTK_INVALID_LIST_POSITION || out_idx == GTK_INVALID_LIST_POSITION ||
            in_p_idx == GTK_INVALID_LIST_POSITION || out_p_idx == GTK_INVALID_LIST_POSITION ||
            in_idx >= dev_list.sources->len || out_idx >= dev_list.sinks->len) {
            gtk_switch_set_active(sw, FALSE);
            return TRUE;
        }

        AudioDevice *src = g_ptr_array_index(dev_list.sources, in_idx);
        AudioDevice *snk = g_ptr_array_index(dev_list.sinks, out_idx);
        const char *src_port = g_ptr_array_index(entry->in_ports, in_p_idx);
        const char *snk_port = g_ptr_array_index(entry->out_ports, out_p_idx);

        // Allocate atomically reference-counted WorkerContext (starts with refcount = 1 for entry)
        WorkerContext *wctx = g_atomic_rc_box_new0(WorkerContext);
        wctx->owner_entry = entry;
        wctx->source_name = g_strdup(src->name);
        wctx->source_port = g_strdup(src_port);
        wctx->sink_name = g_strdup(snk->name);
        wctx->sink_port = g_strdup(snk_port);
        wctx->threshold_db = gtk_spin_button_get_value(GTK_SPIN_BUTTON(entry->thresh_spin));
        g_atomic_int_set(&wctx->running, 1);

        entry->active_worker = wctx;

        gtk_widget_set_sensitive(entry->input_dd, FALSE);
        gtk_widget_set_sensitive(entry->in_port_dd, FALSE);
        gtk_widget_set_sensitive(entry->output_dd, FALSE);
        gtk_widget_set_sensitive(entry->out_port_dd, FALSE);

        // Acquire a second reference specifically for the worker thread
        g_atomic_rc_box_acquire(wctx);
        pthread_t tid;
        if (pthread_create(&tid, NULL, route_worker, wctx) == 0) {
            pthread_detach(tid);
        } else {
            worker_context_unref(wctx); // Release thread ref on failure
            stop_entry(entry);
        }
    } else {
        stop_entry(entry);
        gtk_widget_set_sensitive(entry->input_dd, TRUE);
        gtk_widget_set_sensitive(entry->in_port_dd, TRUE);
        gtk_widget_set_sensitive(entry->output_dd, TRUE);
        gtk_widget_set_sensitive(entry->out_port_dd, TRUE);
    }
    return FALSE;
}

static void on_remove_clicked(GtkButton *btn, gpointer user_data) {
    RouteEntry *entry = (RouteEntry *)user_data;
    stop_entry(entry);
    g_ptr_array_remove(route_entries, entry);
    if (entry->in_ports) g_ptr_array_free(entry->in_ports, TRUE);
    if (entry->out_ports) g_ptr_array_free(entry->out_ports, TRUE);
    GtkWidget *list_row = gtk_widget_get_parent(entry->row_box);
    gtk_list_box_remove(GTK_LIST_BOX(list_box), list_row);
    g_free(entry);
}

static GtkWidget *create_device_dropdown(GPtrArray *devices) {
    const char **strings = g_malloc0((devices->len + 1) * sizeof(char *));
    for (guint i = 0; i < devices->len; i++) {
        AudioDevice *dev = g_ptr_array_index(devices, i);
        strings[i] = dev->description;
    }
    strings[devices->len] = NULL;
    GtkWidget *dd = gtk_drop_down_new_from_strings(strings);
    gtk_widget_set_hexpand(dd, TRUE);
    g_free(strings);
    return dd;
}

static int find_device_index(GPtrArray *devices, const char *node_name) {
    if (!node_name) return -1;
    for (guint i = 0; i < devices->len; i++) {
        AudioDevice *dev = g_ptr_array_index(devices, i);
        if (g_strcmp0(dev->name, node_name) == 0) return (int)i;
    }
    return -1;
}

static int find_port_index(GPtrArray *ports, const char *port_name) {
    if (!port_name || !ports) return -1;
    for (guint i = 0; i < ports->len; i++) {
        const char *p = g_ptr_array_index(ports, i);
        if (g_strcmp0(p, port_name) == 0) return (int)i;
    }
    return -1;
}

static RouteEntry *add_route_row(const char *src_node, const char *src_port,
                                 const char *snk_node, const char *snk_port,
                                 double threshold, bool active) {
    RouteEntry *entry = g_malloc0(sizeof(RouteEntry));

    entry->row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(entry->row_box, 6);
    gtk_widget_set_margin_bottom(entry->row_box, 6);
    gtk_widget_set_margin_start(entry->row_box, 8);
    gtk_widget_set_margin_end(entry->row_box, 8);

    entry->input_dd = create_device_dropdown(dev_list.sources);
    entry->in_port_dd = gtk_drop_down_new(NULL, NULL);
    gtk_widget_set_size_request(entry->in_port_dd, 150, -1);

    entry->output_dd = create_device_dropdown(dev_list.sinks);
    entry->out_port_dd = gtk_drop_down_new(NULL, NULL);
    gtk_widget_set_size_request(entry->out_port_dd, 150, -1);

    g_signal_connect(entry->input_dd, "notify::selected", G_CALLBACK(on_input_node_changed), entry);
    g_signal_connect(entry->output_dd, "notify::selected", G_CALLBACK(on_output_node_changed), entry);

    int src_idx = find_device_index(dev_list.sources, src_node);
    if (src_idx >= 0) gtk_drop_down_set_selected(GTK_DROP_DOWN(entry->input_dd), (guint)src_idx);

    int snk_idx = find_device_index(dev_list.sinks, snk_node);
    if (snk_idx >= 0) gtk_drop_down_set_selected(GTK_DROP_DOWN(entry->output_dd), (guint)snk_idx);

    on_input_node_changed(NULL, NULL, entry);
    on_output_node_changed(NULL, NULL, entry);

    int src_p_idx = find_port_index(entry->in_ports, src_port);
    if (src_p_idx >= 0) gtk_drop_down_set_selected(GTK_DROP_DOWN(entry->in_port_dd), (guint)src_p_idx);

    int snk_p_idx = find_port_index(entry->out_ports, snk_port);
    if (snk_p_idx >= 0) gtk_drop_down_set_selected(GTK_DROP_DOWN(entry->out_port_dd), (guint)snk_p_idx);

    entry->thresh_spin = gtk_spin_button_new_with_range(-80.0, 0.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(entry->thresh_spin), threshold);
    g_signal_connect(entry->thresh_spin, "value-changed", G_CALLBACK(on_thresh_changed), entry);

    entry->enable_switch = gtk_switch_new();
    gtk_widget_set_valign(entry->enable_switch, GTK_ALIGN_CENTER);
    g_signal_connect(entry->enable_switch, "state-set", G_CALLBACK(on_switch_state_set), entry);

    entry->status_label = gtk_label_new("Stopped");
    gtk_widget_set_size_request(entry->status_label, 165, -1);
    gtk_label_set_xalign(GTK_LABEL(entry->status_label), 0.0);

    GtkWidget *remove_btn = gtk_button_new_with_label("Remove");
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(on_remove_clicked), entry);

    gtk_box_append(GTK_BOX(entry->row_box), entry->input_dd);
    gtk_box_append(GTK_BOX(entry->row_box), entry->in_port_dd);
    gtk_box_append(GTK_BOX(entry->row_box), entry->output_dd);
    gtk_box_append(GTK_BOX(entry->row_box), entry->out_port_dd);
    gtk_box_append(GTK_BOX(entry->row_box), entry->thresh_spin);
    gtk_box_append(GTK_BOX(entry->row_box), entry->enable_switch);
    gtk_box_append(GTK_BOX(entry->row_box), entry->status_label);
    gtk_box_append(GTK_BOX(entry->row_box), remove_btn);

    gtk_list_box_append(GTK_LIST_BOX(list_box), entry->row_box);
    g_ptr_array_add(route_entries, entry);

    if (active) {
        gtk_switch_set_active(GTK_SWITCH(entry->enable_switch), TRUE);
    }

    return entry;
}

static void on_add_row_clicked(GtkButton *btn, gpointer user_data) {
    add_route_row(NULL, NULL, NULL, NULL, -40.0, false);
}

static gboolean on_window_close_request(GtkWindow *window, gpointer user_data) {
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    return TRUE;
}

static void on_quit_clicked(GtkButton *btn, gpointer user_data) {
    GApplication *app = G_APPLICATION(user_data);
    perform_clean_shutdown();
    g_application_quit(app);
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    global_app = G_APPLICATION(app);

    if (main_window) {
        gtk_window_present(GTK_WINDOW(main_window));
        return;
    }

    route_entries = g_ptr_array_new();

    g_application_hold(G_APPLICATION(app));

    g_unix_signal_add(SIGINT, on_unix_signal_quit, NULL);
    g_unix_signal_add(SIGTERM, on_unix_signal_quit, NULL);

    register_baked_app_icon();
    setup_system_tray();

    refresh_devices(&dev_list);

    main_window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(main_window), "PipeWire Direct Noise-Gated Patchbay");
    gtk_window_set_icon_name(GTK_WINDOW(main_window), APP_ICON_NAME);
    gtk_window_set_default_size(GTK_WINDOW(main_window), 1220, 460);
    g_signal_connect(main_window, "close-request", G_CALLBACK(on_window_close_request), NULL);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(main_vbox, 12);
    gtk_widget_set_margin_bottom(main_vbox, 12);
    gtk_widget_set_margin_start(main_vbox, 12);
    gtk_widget_set_margin_end(main_vbox, 12);
    gtk_window_set_child(GTK_WINDOW(main_window), main_vbox);

    // Top Action Bar with Toggles
    GtkWidget *top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *add_btn = gtk_button_new_with_label("+ Add Direct Node Link");

    GtkWidget *login_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *login_lbl = gtk_label_new("Launch on login:");
    launch_login_switch = gtk_switch_new();
    gtk_widget_set_valign(launch_login_switch, GTK_ALIGN_CENTER);
    g_signal_connect(launch_login_switch, "state-set", G_CALLBACK(on_launch_login_state_set), NULL);
    gtk_box_append(GTK_BOX(login_box), login_lbl);
    gtk_box_append(GTK_BOX(login_box), launch_login_switch);

    GtkWidget *min_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *min_lbl = gtk_label_new("Start minimized:");
    start_min_switch = gtk_switch_new();
    gtk_widget_set_valign(start_min_switch, GTK_ALIGN_CENTER);
    g_signal_connect(start_min_switch, "state-set", G_CALLBACK(on_start_min_state_set), NULL);
    gtk_box_append(GTK_BOX(min_box), min_lbl);
    gtk_box_append(GTK_BOX(min_box), start_min_switch);

    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);

    GtkWidget *quit_btn = gtk_button_new_with_label("Quit Completely");

    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_row_clicked), NULL);
    g_signal_connect(quit_btn, "clicked", G_CALLBACK(on_quit_clicked), app);

    gtk_box_append(GTK_BOX(top_bar), add_btn);
    gtk_box_append(GTK_BOX(top_bar), login_box);
    gtk_box_append(GTK_BOX(top_bar), min_box);
    gtk_box_append(GTK_BOX(top_bar), spacer);
    gtk_box_append(GTK_BOX(top_bar), quit_btn);
    gtk_box_append(GTK_BOX(main_vbox), top_bar);

    // Table Header
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(header_box, 8);
    gtk_widget_set_margin_end(header_box, 8);

    GtkWidget *h_in = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(h_in), "<b>Input Node</b>");
    gtk_widget_set_hexpand(h_in, TRUE);
    gtk_label_set_xalign(GTK_LABEL(h_in), 0.0);

    GtkWidget *h_in_p = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(h_in_p), "<b>Input Port</b>");
    gtk_widget_set_size_request(h_in_p, 150, -1);
    gtk_label_set_xalign(GTK_LABEL(h_in_p), 0.0);

    GtkWidget *h_out = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(h_out), "<b>Output Node</b>");
    gtk_widget_set_hexpand(h_out, TRUE);
    gtk_label_set_xalign(GTK_LABEL(h_out), 0.0);

    GtkWidget *h_out_p = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(h_out_p), "<b>Output Port</b>");
    gtk_widget_set_size_request(h_out_p, 150, -1);
    gtk_label_set_xalign(GTK_LABEL(h_out_p), 0.0);

    GtkWidget *h_thr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(h_thr), "<b>Gate (dB)</b>");
    gtk_widget_set_size_request(h_thr, 110, -1);

    GtkWidget *h_en = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(h_en), "<b>Active</b>");
    gtk_widget_set_size_request(h_en, 55, -1);

    GtkWidget *h_st = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(h_st), "<b>Status</b>");
    gtk_widget_set_size_request(h_st, 165, -1);
    gtk_label_set_xalign(GTK_LABEL(h_st), 0.0);

    GtkWidget *h_rm = gtk_label_new("");
    gtk_widget_set_size_request(h_rm, 75, -1);

    gtk_box_append(GTK_BOX(header_box), h_in);
    gtk_box_append(GTK_BOX(header_box), h_in_p);
    gtk_box_append(GTK_BOX(header_box), h_out);
    gtk_box_append(GTK_BOX(header_box), h_out_p);
    gtk_box_append(GTK_BOX(header_box), h_thr);
    gtk_box_append(GTK_BOX(header_box), h_en);
    gtk_box_append(GTK_BOX(header_box), h_st);
    gtk_box_append(GTK_BOX(header_box), h_rm);
    gtk_box_append(GTK_BOX(main_vbox), header_box);

    // Scrollable Table Body
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_box), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list_box);
    gtk_box_append(GTK_BOX(main_vbox), scrolled);

    if (!load_config_quietly()) {
        add_route_row(NULL, NULL, NULL, NULL, -40.0, false);
        sync_config_if_diff(true);
    }

    g_timeout_add_seconds(CONFIG_SYNC_INTERVAL_SEC, hourly_config_sync_cb, NULL);

    if (!opt_start_minimized) {
        gtk_window_present(GTK_WINDOW(main_window));
    }
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("org.fedora.audiogaterouter", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
