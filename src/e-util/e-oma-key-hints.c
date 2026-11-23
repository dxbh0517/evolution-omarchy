/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Ctrl key hints: holding Ctrl for a moment shows, over every visible
 * button bound to an action with a keyboard shortcut, a small badge with
 * that shortcut. Shortcuts that need Ctrl show only the rest of the
 * combination ("R" for Ctrl+R, "⇧R" for Shift+Ctrl+R); others (Alt+Home,
 * Delete) show in full on a quieter badge. Releasing Ctrl, pressing a
 * shortcut or leaving the window hides them; with Ctrl still held after a
 * shortcut, they come back for whatever is now on screen.
 */

#include "evolution-config.h"

#include <string.h>

#include "e-ui-action.h"
#include "e-oma-key-hints.h"

#define SHOW_DELAY_MS 300
#define RESHOW_DELAY_MS 150
#define HINT_DATA "oma-key-hint-badge"

typedef struct _KeyHints {
	GtkWindow *window; /* weak */
	GtkOverlay *overlay; /* not referenced; owns this struct */
	GSList *badges;
	guint show_id;
	gulong handler_ids[3];
} KeyHints;

typedef struct _BadgeGeom {
	gint x, y, width, height;
} BadgeGeom;

static void
key_hints_ensure_style (void)
{
	static gboolean done = FALSE;
	GtkCssProvider *provider;

	if (done)
		return;
	done = TRUE;

	/* Neutral fallback; omamail.css (higher priority) gives the themed look. */
	provider = gtk_css_provider_new ();
	gtk_css_provider_load_from_data (provider,
		".oma-key-hint {"
		"  background-color: @theme_selected_bg_color;"
		"  color: @theme_selected_fg_color;"
		"  border-radius: 5px;"
		"  padding: 1px 5px;"
		"  font-size: 8pt;"
		"  font-weight: bold;"
		"  box-shadow: 0 1px 3px rgba(0,0,0,0.45);"
		"}"
		".oma-key-hint.oma-key-hint-other {"
		"  background-color: @theme_bg_color;"
		"  color: @theme_fg_color;"
		"  border: 1px solid @borders;"
		"}", -1, NULL);
	gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
		GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref (provider);
}

static GAction *
key_hints_lookup_in_group (GActionGroup *group,
                           const gchar *name)
{
	if (!group || !G_IS_ACTION_MAP (group))
		return NULL;

	return g_action_map_lookup_action (G_ACTION_MAP (group), name);
}

/* Finds the action a widget triggers, looking through the action groups
 * inserted on it and its ancestors, the way GtkActionable resolves it. */
static EUIAction *
key_hints_find_action (GtkWidget *widget,
                       const gchar *full_name)
{
	const gchar *dot = strchr (full_name, '.');
	GtkWidget *anc;

	for (anc = widget; anc; anc = gtk_widget_get_parent (anc)) {
		GAction *action = NULL;

		if (dot) {
			gchar *prefix = g_strndup (full_name, dot - full_name);

			action = key_hints_lookup_in_group (gtk_widget_get_action_group (anc, prefix), dot + 1);
			g_free (prefix);
		} else {
			/* A bare action name: try every group on this level. */
			const gchar **prefixes = gtk_widget_list_action_prefixes (anc);
			gint ii;

			for (ii = 0; prefixes && prefixes[ii] && !action; ii++)
				action = key_hints_lookup_in_group (gtk_widget_get_action_group (anc, prefixes[ii]), full_name);

			g_free (prefixes);
		}

		if (action)
			return E_IS_UI_ACTION (action) ? E_UI_ACTION (action) : NULL;
	}

	return NULL;
}

/* Ctrl shortcuts drop the "Ctrl+" the user is already holding. */
static gchar *
key_hints_format (const gchar *accel,
                  gboolean *out_with_ctrl)
{
	GdkModifierType mods = 0;
	guint key = 0;
	GString *text;
	gchar *key_label;

	gtk_accelerator_parse (accel, &key, &mods);
	if (!key)
		return NULL;

	*out_with_ctrl = (mods & GDK_CONTROL_MASK) != 0;

	text = g_string_new (NULL);
	if (!*out_with_ctrl && (mods & GDK_SUPER_MASK))
		g_string_append (text, "Super+");
	if (mods & GDK_MOD1_MASK)
		g_string_append (text, "Alt+");
	if (mods & GDK_SHIFT_MASK)
		g_string_append (text, *out_with_ctrl ? "⇧" : "Shift+");

	key_label = gtk_accelerator_get_label (key, 0);
	g_string_append (text, key_label);
	g_free (key_label);

	return g_string_free (text, FALSE);
}

static gboolean
key_hints_add_badge (KeyHints *kh,
                     GtkWidget *target,
                     EUIAction *action)
{
	GtkWidget *overlay = GTK_WIDGET (kh->overlay);
	const gchar *accel;
	GtkWidget *badge;
	BadgeGeom *geom;
	gboolean with_ctrl = FALSE;
	gchar *text;
	gint x = 0, y = 0;

	if (!e_ui_action_get_visible (action) || !gtk_widget_is_sensitive (target))
		return FALSE;

	accel = e_ui_action_get_accel (action);
	if (!accel || !*accel)
		accel = g_object_get_data (G_OBJECT (action), E_OMA_KEY_HINT_ACCEL);
	if (!accel || !*accel)
		return FALSE;

	if (!gtk_widget_translate_coordinates (target, overlay, 0, 0, &x, &y))
		return FALSE;

	/* Off-screen, e.g. scrolled out of a viewport. */
	if (x + gtk_widget_get_allocated_width (target) <= 0 ||
	    y + gtk_widget_get_allocated_height (target) <= 0 ||
	    x >= gtk_widget_get_allocated_width (overlay) ||
	    y >= gtk_widget_get_allocated_height (overlay))
		return FALSE;

	text = key_hints_format (accel, &with_ctrl);
	if (!text)
		return FALSE;

	badge = gtk_label_new (text);
	g_free (text);
	gtk_style_context_add_class (gtk_widget_get_style_context (badge), "oma-key-hint");
	if (!with_ctrl)
		gtk_style_context_add_class (gtk_widget_get_style_context (badge), "oma-key-hint-other");

	geom = g_new0 (BadgeGeom, 1);
	geom->x = x;
	geom->y = y;
	geom->width = gtk_widget_get_allocated_width (target);
	geom->height = gtk_widget_get_allocated_height (target);
	g_object_set_data_full (G_OBJECT (badge), HINT_DATA, geom, g_free);

	gtk_overlay_add_overlay (kh->overlay, badge);
	gtk_overlay_set_overlay_pass_through (kh->overlay, badge, TRUE);
	gtk_widget_show (badge);

	kh->badges = g_slist_prepend (kh->badges, badge);

	return TRUE;
}

static void key_hints_collect (GtkWidget *widget, gpointer user_data);

static void
key_hints_collect (GtkWidget *widget,
                   gpointer user_data)
{
	KeyHints *kh = user_data;

	if (!gtk_widget_is_drawable (widget) ||
	    g_object_get_data (G_OBJECT (widget), HINT_DATA))
		return;

	if (GTK_IS_ACTIONABLE (widget)) {
		const gchar *name = gtk_actionable_get_action_name (GTK_ACTIONABLE (widget));
		EUIAction *action = name ? key_hints_find_action (widget, name) : NULL;

		if (action && key_hints_add_badge (kh, widget, action))
			return;
	} else {
		/* Plain buttons that activate an action by name (row quick actions). */
		const gchar *name = g_object_get_data (G_OBJECT (widget), "oma-action-name");
		EUIAction *action = name ? key_hints_find_action (widget, name) : NULL;

		if (action && key_hints_add_badge (kh, widget, action))
			return;
	}

	if (GTK_IS_CONTAINER (widget))
		gtk_container_forall (GTK_CONTAINER (widget), key_hints_collect, kh);
}

static gboolean
key_hints_get_child_position_cb (GtkOverlay *overlay,
                                 GtkWidget *child,
                                 GdkRectangle *allocation,
                                 gpointer user_data)
{
	BadgeGeom *geom = g_object_get_data (G_OBJECT (child), HINT_DATA);
	GtkRequisition natural;
	gint max_x, max_y;

	if (!geom)
		return FALSE;

	gtk_widget_get_preferred_size (child, NULL, &natural);

	/* Centered on the bottom edge of the button, kept inside the window. */
	allocation->width = natural.width;
	allocation->height = natural.height;
	allocation->x = geom->x + (geom->width - natural.width) / 2;
	allocation->y = geom->y + geom->height - (natural.height * 2) / 3;

	max_x = gtk_widget_get_allocated_width (GTK_WIDGET (overlay)) - natural.width - 2;
	max_y = gtk_widget_get_allocated_height (GTK_WIDGET (overlay)) - natural.height - 2;
	allocation->x = CLAMP (allocation->x, 2, MAX (2, max_x));
	allocation->y = CLAMP (allocation->y, 2, MAX (2, max_y));

	return TRUE;
}

static void
key_hints_hide (KeyHints *kh)
{
	GSList *badges = kh->badges;

	kh->badges = NULL;
	g_slist_free_full (badges, (GDestroyNotify) gtk_widget_destroy);
}

static void
key_hints_cancel (KeyHints *kh)
{
	if (kh->show_id) {
		g_source_remove (kh->show_id);
		kh->show_id = 0;
	}
}

static gboolean
key_hints_ctrl_held (KeyHints *kh)
{
	GdkKeymap *keymap;

	keymap = gdk_keymap_get_for_display (gtk_widget_get_display (GTK_WIDGET (kh->overlay)));

	return (gdk_keymap_get_modifier_state (keymap) & GDK_CONTROL_MASK) != 0;
}

static gboolean
key_hints_show_cb (gpointer user_data)
{
	KeyHints *kh = user_data;

	kh->show_id = 0;

	if (!kh->window || !gtk_window_is_active (kh->window) || !key_hints_ctrl_held (kh))
		return G_SOURCE_REMOVE;

	key_hints_hide (kh);
	key_hints_ensure_style ();
	key_hints_collect (gtk_bin_get_child (GTK_BIN (kh->overlay)), kh);

	return G_SOURCE_REMOVE;
}

static void
key_hints_schedule (KeyHints *kh,
                    guint delay)
{
	if (!kh->show_id && !kh->badges)
		kh->show_id = g_timeout_add (delay, key_hints_show_cb, kh);
}

static gboolean
key_hints_is_ctrl (guint keyval)
{
	return keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R;
}

static gboolean
key_hints_key_press_cb (GtkWidget *window,
                        GdkEventKey *event,
                        gpointer user_data)
{
	KeyHints *kh = user_data;

	if (key_hints_is_ctrl (event->keyval)) {
		/* Ctrl alone; Ctrl added to Shift/Alt is a chord in progress. */
		if (!(event->state & (GDK_SHIFT_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK)))
			key_hints_schedule (kh, SHOW_DELAY_MS);
	} else if (!event->is_modifier) {
		/* A shortcut (or typing): get out of the way. */
		key_hints_cancel (kh);
		key_hints_hide (kh);
	}

	return FALSE;
}

static gboolean
key_hints_key_release_cb (GtkWidget *window,
                          GdkEventKey *event,
                          gpointer user_data)
{
	KeyHints *kh = user_data;

	if (key_hints_is_ctrl (event->keyval)) {
		key_hints_cancel (kh);
		key_hints_hide (kh);
	} else if (!event->is_modifier && (event->state & GDK_CONTROL_MASK)) {
		/* Ctrl still held after a shortcut: show what is there now. */
		key_hints_schedule (kh, RESHOW_DELAY_MS);
	}

	return FALSE;
}

static gboolean
key_hints_focus_out_cb (GtkWidget *window,
                        GdkEvent *event,
                        gpointer user_data)
{
	KeyHints *kh = user_data;

	key_hints_cancel (kh);
	key_hints_hide (kh);

	return FALSE;
}

static void
key_hints_free (gpointer ptr)
{
	KeyHints *kh = ptr;
	gint ii;

	key_hints_cancel (kh);
	/* The badges go with the overlay. */
	g_slist_free (kh->badges);

	if (kh->window) {
		for (ii = 0; ii < G_N_ELEMENTS (kh->handler_ids); ii++) {
			if (kh->handler_ids[ii])
				g_signal_handler_disconnect (kh->window, kh->handler_ids[ii]);
		}
		g_object_remove_weak_pointer (G_OBJECT (kh->window), (gpointer *) &kh->window);
	}

	g_free (kh);
}

/**
 * e_oma_key_hints_attach:
 * @window: the toplevel receiving key events
 * @overlay: a #GtkOverlay inside @window whose child holds the controls
 *
 * Shows shortcut badges over the controls of @overlay while Ctrl is held.
 **/
void
e_oma_key_hints_attach (GtkWindow *window,
                        GtkOverlay *overlay)
{
	KeyHints *kh;

	g_return_if_fail (GTK_IS_WINDOW (window));
	g_return_if_fail (GTK_IS_OVERLAY (overlay));

	kh = g_new0 (KeyHints, 1);
	kh->window = window;
	kh->overlay = overlay;
	g_object_add_weak_pointer (G_OBJECT (window), (gpointer *) &kh->window);

	kh->handler_ids[0] = g_signal_connect (window, "key-press-event",
		G_CALLBACK (key_hints_key_press_cb), kh);
	kh->handler_ids[1] = g_signal_connect (window, "key-release-event",
		G_CALLBACK (key_hints_key_release_cb), kh);
	kh->handler_ids[2] = g_signal_connect (window, "focus-out-event",
		G_CALLBACK (key_hints_focus_out_cb), kh);

	g_signal_connect (overlay, "get-child-position",
		G_CALLBACK (key_hints_get_child_position_cb), NULL);

	g_object_set_data_full (G_OBJECT (overlay), "oma-key-hints", kh, key_hints_free);
}
