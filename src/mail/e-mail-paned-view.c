/*
 * SPDX-FileCopyrightText: (C) 2010 Intel corporation. (www.intel.com)
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileContributor: Srinivasa Ragavan <sragavan@gnome.org>
 */

#include "evolution-config.h"

#include <glib/gi18n.h>

#include <shell/e-shell-window-actions.h>

#include <libemail-engine/libemail-engine.h>

#include "em-utils.h"
#include "message-list.h"
#include "e-mail-reader-utils.h"

#include "e-mail-paned-view.h"
#include <libgnomecanvas/libgnomecanvas.h>

#define E_SHELL_WINDOW_ACTION_GROUP_MAIL(window) \
	E_SHELL_WINDOW_ACTION_GROUP ((window), "mail")

struct _EMailPanedViewPrivate {
	GtkWidget *paned;
	GtkWidget *scrolled_window;

	/* Omarchy list column: title header and filter chips above the list */
	GtkWidget *list_column;
	GtkWidget *list_title;
	GtkWidget *list_subtitle;
	GtkWidget *list_filters_box;
	GtkWidget *message_list;
	GtkWidget *preview_pane;
	GtkWidget *preview_toolbar_box;

	EMailDisplay *display;
	GalViewInstance *view_instance;

	/* ETable scrolling hack */
	gdouble default_scrollbar_position;

	guint paned_binding_id;

	/* Signal handler IDs */
	guint message_list_built_id;

	/* TRUE when folder had been just set */
	gboolean folder_just_set;
	gchar *last_selected_uid;

	gboolean preview_toolbar_visible;
	guint responsive_idle;
	gboolean responsive_stacked;

	/* Omarchy quick selection */
	GtkWidget *list_overlay;
	GtkWidget *selection_revealer;
	GtkWidget *selection_label;
	GtkWidget *selection_actions;
	gboolean select_mode;

	/* Omarchy per-row quick actions, shown on the hovered message */
	GtkWidget *row_actions;
	GtkWidget *row_read_button;
	GtkWidget *row_read_image;
	GtkWidget *row_flag_button;
	gchar *row_uid;
	gint row_index;
	GdkRectangle row_rect;	/* in list_overlay coordinates */
	gboolean row_pointer_inside;
	guint row_hide_id;
};

enum {
	PROP_0,
	PROP_PREVIEW_TOOLBAR_VISIBLE,
	N_PROPS,
	PROP_FORWARD_STYLE,
	PROP_GROUP_BY_THREADS,
	PROP_REPLY_STYLE,
	PROP_MARK_SEEN_ALWAYS,
	PROP_DELETE_SELECTS_PREVIOUS,
};

static GParamSpec *properties[N_PROPS] = { NULL, };

#define STATE_KEY_GROUP_BY_THREADS	"GroupByThreads"
#define STATE_KEY_SELECTED_MESSAGE	"SelectedMessage"
#define STATE_KEY_PREVIEW_VISIBLE	"PreviewVisible"
#define STATE_GROUP_GLOBAL_FOLDER	"GlobalFolder"

/* Forward Declarations */
static void e_mail_paned_view_reader_init (EMailReaderInterface *iface);
static void mail_paned_view_responsive_size_allocate_cb (GtkWidget *widget, GtkAllocation *allocation, gpointer user_data);
static void mail_paned_view_update_list_title (EMailPanedView *self, CamelFolder *folder);

G_DEFINE_TYPE_WITH_CODE (EMailPanedView, e_mail_paned_view, E_TYPE_MAIL_VIEW,
	G_ADD_PRIVATE (EMailPanedView)
	G_IMPLEMENT_INTERFACE (E_TYPE_MAIL_READER, e_mail_paned_view_reader_init)
	G_IMPLEMENT_INTERFACE (E_TYPE_EXTENSIBLE, NULL))

static void
mail_paned_view_save_boolean (EMailView *view,
                              const gchar *key,
                              gboolean value)
{
	EMailReader *reader;
	CamelFolder *folder;

	reader = E_MAIL_READER (view);
	folder = e_mail_reader_ref_folder (reader);

	if (folder != NULL) {
		EShellView *shell_view;
		GKeyFile *key_file;
		gchar *folder_uri;
		gchar *group_name;

		shell_view = e_mail_view_get_shell_view (view);
		key_file = e_shell_view_get_state_key_file (shell_view);

		folder_uri = e_mail_folder_uri_from_folder (folder);
		group_name = g_strdup_printf ("Folder %s", folder_uri);
		g_key_file_set_boolean (key_file, group_name, key, value);
		g_free (group_name);
		g_free (folder_uri);

		g_key_file_set_boolean (
			key_file, STATE_GROUP_GLOBAL_FOLDER, key, value);

		e_shell_view_set_state_dirty (shell_view);

		g_object_unref (folder);
	}
}

static gboolean
mail_paned_view_message_list_is_empty (MessageList *message_list)
{
	ETreeModel *model;
	ETreePath root;

	g_return_val_if_fail (IS_MESSAGE_LIST (message_list), TRUE);

	model = e_tree_get_model (E_TREE (message_list));
	if (!model)
		return TRUE;

	root = e_tree_model_get_root (model);
	if (!root)
		return TRUE;

	return !e_tree_model_node_get_first_child (model, root);
}

static void
mail_paned_view_message_list_built_cb (EMailView *view,
                                       MessageList *message_list)
{
	EMailPanedView *self = E_MAIL_PANED_VIEW (view);
	EShellView *shell_view;
	EShellWindow *shell_window;
	CamelFolder *folder;
	GKeyFile *key_file;
	gboolean ensure_message_selected;

	ensure_message_selected = self->priv->folder_just_set;
	self->priv->folder_just_set = FALSE;

	folder = message_list_ref_folder (message_list);

	shell_view = e_mail_view_get_shell_view (view);
	shell_window = e_shell_view_get_shell_window (shell_view);

	key_file = e_shell_view_get_state_key_file (shell_view);

	if (message_list->cursor_uid != NULL)
		;  /* do nothing */

	else if (folder == NULL)
		;  /* do nothing */

	else if (e_shell_window_get_safe_mode (shell_window))
		e_shell_window_set_safe_mode (shell_window, FALSE);

	else {
		gboolean with_fallback = TRUE;
		gchar *uid = NULL;

		/* This is for regen when setting filter, or when folder changed or such */
		if (!ensure_message_selected &&
		    !message_list_selected_count (message_list) &&
		    !mail_paned_view_message_list_is_empty (message_list)) {
			ensure_message_selected = TRUE;
			with_fallback = FALSE;

			if (self->priv->last_selected_uid &&
			    message_list_contains_uid (message_list, self->priv->last_selected_uid)) {
				g_free (uid);
				uid = g_strdup (self->priv->last_selected_uid);
			}
		}

		/* This is to prefer last selected message from the previous search folder
		   over the stored message. The _set_folder() makes sure to unset
		   priv->last_selected_uid, when it's not from this folder. */
		if (ensure_message_selected && !uid && self->priv->last_selected_uid &&
		    message_list_contains_uid (message_list, self->priv->last_selected_uid)) {
			uid = g_strdup (self->priv->last_selected_uid);
		}

		if (ensure_message_selected && !uid) {
			const gchar *key;
			gchar *folder_uri;
			gchar *group_name;

			folder_uri = e_mail_folder_uri_from_folder (folder);

			key = STATE_KEY_SELECTED_MESSAGE;
			group_name = g_strdup_printf ("Folder %s", folder_uri);
			uid = g_key_file_get_string (key_file, group_name, key, NULL);
			g_free (group_name);

			g_free (folder_uri);
		}

		if (ensure_message_selected && !message_list_contains_uid (message_list, uid) &&
		    e_mail_reader_get_mark_seen_always (E_MAIL_READER (view)))
			e_mail_reader_unset_folder_just_selected (E_MAIL_READER (view));

		if (ensure_message_selected)
			message_list_select_uid (message_list, uid, with_fallback);

		g_free (uid);
	}

	g_clear_object (&folder);
}

static void
mail_paned_view_message_selected_cb (EMailView *view,
                                     const gchar *message_uid,
                                     MessageList *message_list)
{
	EShellView *shell_view;
	CamelFolder *folder;
	GKeyFile *key_file;
	const gchar *key;
	gchar *folder_uri;
	gchar *group_name;

	mail_paned_view_responsive_size_allocate_cb (GTK_WIDGET (view), NULL, NULL);
	folder = message_list_ref_folder (message_list);

	/* This also gets triggered when selecting a store name on
	 * the sidebar such as "On This Computer", in which case
	 * 'folder' will be NULL. */
	if (folder == NULL)
		return;

	shell_view = e_mail_view_get_shell_view (view);
	key_file = e_shell_view_get_state_key_file (shell_view);

	folder_uri = e_mail_folder_uri_from_folder (folder);

	key = STATE_KEY_SELECTED_MESSAGE;
	group_name = g_strdup_printf ("Folder %s", folder_uri);

	/* Only overwrite changes, do not delete the stored selected message,
	   when the current view has nothing (or multiple messages) selected. */
	if (message_uid != NULL) {
		EMailPanedView *paned_view = E_MAIL_PANED_VIEW (view);

		g_key_file_set_string (key_file, group_name, key, message_uid);

		g_clear_pointer (&paned_view->priv->last_selected_uid, g_free);
		paned_view->priv->last_selected_uid = g_strdup (message_uid);
	}

	e_shell_view_set_state_dirty (shell_view);

	g_free (group_name);
	g_free (folder_uri);

	g_object_unref (folder);
}

/* To recognize old values from new values */
#define PROPORTION_LOWER_LIMIT 1000000

static gboolean
mail_paned_view_map_setting_to_proportion_cb (GValue *value,
					      GVariant *variant,
					      gpointer user_data)
{
	gint stored;
	gdouble proportion = 0.5;

	stored = g_variant_get_int32 (variant);

	if (stored >= PROPORTION_LOWER_LIMIT)
		proportion = (stored - PROPORTION_LOWER_LIMIT) / ((gdouble) PROPORTION_LOWER_LIMIT);

	g_value_set_double (value, proportion);

	return TRUE;
}

static GVariant *
mail_paned_view_map_proportion_to_setting_cb (const GValue *value,
					      const GVariantType *expected_type,
					      gpointer user_data)
{
	gdouble proportion;

	proportion = g_value_get_double (value);

	return g_variant_new_int32 (PROPORTION_LOWER_LIMIT + (gint32) (proportion * PROPORTION_LOWER_LIMIT));
}

static void
mail_paned_view_notify_orientation_cb (GtkWidget *paned,
				       GParamSpec *param,
				       EShellWindow *shell_window)
{
	GSettings *settings;
	const gchar *settings_key;
	guint32 add_flags = 0;

	g_return_if_fail (E_IS_PANED (paned));
	g_return_if_fail (E_IS_SHELL_WINDOW (shell_window));

	g_settings_unbind (paned, "proportion");
	/* Narrow layouts have a temporary split, never a saved desktop ratio. */
	if (g_object_get_data (G_OBJECT (paned), "oma-responsive-stacked"))
		return;

	if (e_shell_window_is_main_instance (shell_window)) {
		if (gtk_orientable_get_orientation (GTK_ORIENTABLE (paned)) == GTK_ORIENTATION_HORIZONTAL)
			settings_key = "hpaned-size";
		else
			settings_key = "paned-size";
	} else {
		if (gtk_orientable_get_orientation (GTK_ORIENTABLE (paned)) == GTK_ORIENTATION_HORIZONTAL)
			settings_key = "hpaned-size-sub";
		else
			settings_key = "paned-size-sub";

		add_flags = G_SETTINGS_BIND_GET_NO_CHANGES;
	}

	settings = e_util_ref_settings ("org.gnome.evolution.mail");

	g_settings_bind_with_mapping (settings, settings_key,
		paned, "proportion",
		G_SETTINGS_BIND_DEFAULT | add_flags,
		mail_paned_view_map_setting_to_proportion_cb,
		mail_paned_view_map_proportion_to_setting_cb,
		NULL, NULL);

	g_object_unref (settings);
}

static void
mail_paned_view_restore_state_cb (EShellWindow *shell_window,
                                  EShellView *shell_view,
                                  EMailPanedView *view)
{
	EMailPanedViewPrivate *priv;

	priv = E_MAIL_PANED_VIEW (view)->priv;

	g_signal_connect (priv->paned, "notify::orientation",
		G_CALLBACK (mail_paned_view_notify_orientation_cb), shell_window);

	mail_paned_view_notify_orientation_cb (priv->paned, NULL, shell_window);
}

/* Use native panes and actions at every size; only their presentation changes. */
static void
mail_paned_view_compact_header (GtkWidget *widget,
                               gpointer user_data)
{
	gint width = GPOINTER_TO_INT (user_data);
	if (E_IS_HEADER_BAR_BUTTON (widget)) {
		EUIAction *action = NULL;
		const gchar *name;
		gboolean secondary, hidden;
		g_object_get (widget, "action", &action, NULL);
		if (!action)
			return;
		name = g_action_get_name (G_ACTION (action));
		secondary = g_str_equal (name, "mail-jump-top") ||
			g_str_equal (name, "mail-reply-all") ||
			g_str_equal (name, "mail-forward") ||
			g_str_equal (name, "oma-mail-flag-toggle") ||
			g_str_equal (name, "mail-archive");
		hidden = (secondary && width < 1000) ||
			(g_str_equal (name, "oma-unread-filter") && width < 600);
		if (secondary || g_str_equal (name, "oma-unread-filter")) {
			gtk_widget_set_no_show_all (widget, hidden);
			gtk_widget_set_visible (widget, !hidden && e_ui_action_is_visible (action));
		}
		g_object_unref (action);
		return;
	}
	if (GTK_IS_LABEL (widget) &&
	    gtk_style_context_has_class (gtk_widget_get_style_context (widget), "title")) {
		gtk_label_set_ellipsize (GTK_LABEL (widget), PANGO_ELLIPSIZE_END);
		gtk_label_set_max_width_chars (GTK_LABEL (widget), width < 600 ? 6 : (width < 1000 ? 12 : 30));
	}
	if (GTK_IS_CONTAINER (widget))
		gtk_container_forall (GTK_CONTAINER (widget), mail_paned_view_compact_header, user_data);
}

static gboolean
mail_paned_view_responsive_idle_cb (gpointer user_data)
{
	EMailPanedView *self = E_MAIL_PANED_VIEW (user_data);
	EShellView *shell_view = e_mail_view_get_shell_view (E_MAIL_VIEW (self));
	GtkWidget *window, *header;
	GtkOrientation requested, target;
	gboolean stacked, changed, preview_visible;
	self->priv->responsive_idle = 0;
	if (!shell_view || !self->priv->paned)
		return G_SOURCE_REMOVE;
	window = gtk_widget_get_toplevel (GTK_WIDGET (self));
	requested = gtk_orientable_get_orientation (GTK_ORIENTABLE (self));
	stacked = requested == GTK_ORIENTATION_HORIZONTAL &&
		gtk_widget_get_allocated_width (GTK_WIDGET (self)) < 840;
	target = stacked ? GTK_ORIENTATION_VERTICAL : requested;
	changed = stacked != self->priv->responsive_stacked;
	if (changed || gtk_orientable_get_orientation (GTK_ORIENTABLE (self->priv->paned)) != target) {
		EShellWindow *shell_window = e_shell_view_get_shell_window (shell_view);
		/* EPaned can notify its ratio during the orientation change itself. */
		g_settings_unbind (self->priv->paned, "proportion");
		self->priv->responsive_stacked = stacked;
		g_object_set_data (G_OBJECT (self->priv->paned), "oma-responsive-stacked", GINT_TO_POINTER (stacked));
		gtk_orientable_set_orientation (GTK_ORIENTABLE (self->priv->paned), target);
		mail_paned_view_notify_orientation_cb (self->priv->paned, NULL, shell_window);
		if (stacked)
			e_paned_set_proportion (E_PANED (self->priv->paned), 0.60);
	}
	g_object_get (self, "preview-visible", &preview_visible, NULL);
	/* Do not reserve half a small window for an empty reading pane. */
	gtk_widget_set_visible (gtk_paned_get_child2 (GTK_PANED (self->priv->paned)),
		preview_visible && (!stacked || message_list_selected_count (MESSAGE_LIST (self->priv->message_list)) > 0));
	header = e_shell_view_get_headerbar (shell_view);
	if (header)
		mail_paned_view_compact_header (header, GINT_TO_POINTER (gtk_widget_get_allocated_width (window)));
	return G_SOURCE_REMOVE;
}

static void
mail_paned_view_responsive_size_allocate_cb (GtkWidget *widget,
                                            GtkAllocation *allocation,
                                            gpointer user_data)
{
	EMailPanedView *self = E_MAIL_PANED_VIEW (widget);
	/* Defer orientation changes to avoid recursive allocations during tiling. */
	if (!self->priv->responsive_idle)
		self->priv->responsive_idle = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
			mail_paned_view_responsive_idle_cb, g_object_ref (self), g_object_unref);
}

/* —— Omarchy quick selection ——
 * A floating bar at the bottom of the message list, shown while several
 * messages are selected or while "Select" mode is on. In "Select" mode a
 * plain click toggles a message in or out of the selection, like a
 * checkbox, so a batch can be picked without holding Ctrl. */

static void
mail_paned_view_update_selection_bar (EMailPanedView *self)
{
	gint count;
	gchar *text;

	if (!self->priv->selection_revealer || !self->priv->message_list)
		return;

	count = message_list_selected_count (MESSAGE_LIST (self->priv->message_list));

	if (count > 0)
		text = g_strdup_printf (ngettext ("%d selected", "%d selected", count), count);
	else
		text = g_strdup (_("Select messages"));

	gtk_label_set_text (GTK_LABEL (self->priv->selection_label), text);
	gtk_widget_set_sensitive (self->priv->selection_actions, count > 0);
	gtk_revealer_set_reveal_child (GTK_REVEALER (self->priv->selection_revealer),
		self->priv->select_mode || count > 1);

	g_free (text);
}

static void
mail_paned_view_selection_change_cb (ETree *tree,
				     gpointer user_data)
{
	EMailPanedView *self = E_MAIL_PANED_VIEW (user_data);

	mail_paned_view_update_selection_bar (self);

	if (self->priv->row_actions && self->priv->row_index >= 0) {
		GtkStyleContext *context = gtk_widget_get_style_context (self->priv->row_actions);

		if (e_selection_model_is_row_selected (e_tree_get_selection_model (tree), self->priv->row_index))
			gtk_style_context_add_class (context, "oma-on-selected");
		else
			gtk_style_context_remove_class (context, "oma-on-selected");
	}
}

static void
mail_paned_view_selection_action_clicked_cb (GtkButton *button,
					     gpointer user_data)
{
	EMailPanedView *self = E_MAIL_PANED_VIEW (user_data);
	const gchar *action_name;
	EUIAction *action;

	action_name = g_object_get_data (G_OBJECT (button), "oma-action-name");
	action = action_name ? e_mail_reader_get_action (E_MAIL_READER (self), action_name) : NULL;

	if (action && e_ui_action_get_sensitive (action))
		g_action_activate (G_ACTION (action), NULL);

	mail_paned_view_update_selection_bar (self);
}

static void
mail_paned_view_select_all_clicked_cb (GtkButton *button,
				       gpointer user_data)
{
	EMailPanedView *self = E_MAIL_PANED_VIEW (user_data);

	message_list_select_all (MESSAGE_LIST (self->priv->message_list));
	gtk_widget_grab_focus (self->priv->message_list);
}

static void
mail_paned_view_selection_done_clicked_cb (GtkButton *button,
					   gpointer user_data)
{
	EMailPanedView *self = E_MAIL_PANED_VIEW (user_data);
	ESelectionModel *selection;
	EShellView *shell_view;
	EUIAction *action = NULL;
	gint cursor_row;

	/* Keep only the message under the cursor selected. */
	selection = e_tree_get_selection_model (E_TREE (self->priv->message_list));
	cursor_row = e_selection_model_cursor_row (selection);
	if (cursor_row >= 0)
		e_selection_model_select_single_row (selection, cursor_row);
	else
		e_selection_model_clear (selection);

	shell_view = e_mail_view_get_shell_view (E_MAIL_VIEW (self));
	if (shell_view)
		action = e_shell_view_get_action (shell_view, "oma-select-mode");

	if (action && e_ui_action_get_active (action))
		g_action_change_state (G_ACTION (action), g_variant_new_boolean (FALSE));
	else
		e_mail_paned_view_set_select_mode (self, FALSE);
}

static GtkWidget *
mail_paned_view_selection_button (EMailPanedView *self,
				  const gchar *icon_name,
				  const gchar *label,
				  const gchar *tooltip,
				  const gchar *action_name)
{
	GtkWidget *button, *box, *widget;

	button = gtk_button_new ();
	gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text (button, tooltip);
	gtk_widget_set_can_focus (button, FALSE);

	box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_container_add (GTK_CONTAINER (button), box);

	widget = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_BUTTON);
	gtk_box_pack_start (GTK_BOX (box), widget, FALSE, FALSE, 0);

	if (label) {
		widget = gtk_label_new (label);
		gtk_box_pack_start (GTK_BOX (box), widget, FALSE, FALSE, 0);
	}

	gtk_widget_show_all (button);

	if (action_name) {
		g_object_set_data (G_OBJECT (button), "oma-action-name", (gpointer) action_name);
		g_signal_connect (button, "clicked",
			G_CALLBACK (mail_paned_view_selection_action_clicked_cb), self);
	}

	return button;
}

static void
mail_paned_view_build_selection_bar (EMailPanedView *self)
{
	GtkWidget *revealer, *bar, *actions, *widget;

	revealer = gtk_revealer_new ();
	gtk_revealer_set_transition_type (GTK_REVEALER (revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
	gtk_revealer_set_transition_duration (GTK_REVEALER (revealer), 160);
	gtk_widget_set_halign (revealer, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (revealer, GTK_ALIGN_END);
	gtk_widget_set_margin_bottom (revealer, 16);
	gtk_widget_set_margin_start (revealer, 8);
	gtk_widget_set_margin_end (revealer, 8);
	gtk_overlay_add_overlay (GTK_OVERLAY (self->priv->list_overlay), revealer);
	gtk_overlay_set_overlay_pass_through (GTK_OVERLAY (self->priv->list_overlay), revealer, FALSE);
	self->priv->selection_revealer = revealer;
	gtk_widget_show (revealer);

	bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_widget_set_name (bar, "omamail-selection-bar");
	gtk_style_context_add_class (gtk_widget_get_style_context (bar), "oma-selection-bar");
	gtk_container_add (GTK_CONTAINER (revealer), bar);
	gtk_widget_show (bar);

	widget = gtk_label_new (NULL);
	gtk_style_context_add_class (gtk_widget_get_style_context (widget), "oma-selection-count");
	gtk_label_set_width_chars (GTK_LABEL (widget), 10);
	gtk_label_set_xalign (GTK_LABEL (widget), 0.0);
	gtk_widget_set_margin_start (widget, 8);
	gtk_widget_set_margin_end (widget, 4);
	gtk_box_pack_start (GTK_BOX (bar), widget, FALSE, FALSE, 0);
	self->priv->selection_label = widget;
	gtk_widget_show (widget);

	widget = mail_paned_view_selection_button (self, "edit-select-all-symbolic", NULL, _("Select all messages"), NULL);
	g_signal_connect (widget, "clicked", G_CALLBACK (mail_paned_view_select_all_clicked_cb), self);
	gtk_box_pack_start (GTK_BOX (bar), widget, FALSE, FALSE, 0);

	actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_box_pack_start (GTK_BOX (bar), actions, FALSE, FALSE, 0);
	self->priv->selection_actions = actions;
	gtk_widget_show (actions);

	widget = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
	gtk_widget_set_margin_start (widget, 4);
	gtk_widget_set_margin_end (widget, 4);
	gtk_box_pack_start (GTK_BOX (actions), widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);

	gtk_box_pack_start (GTK_BOX (actions), mail_paned_view_selection_button (self,
		"mail-read-symbolic", _("Read"), _("Mark the selected messages as read"), "mail-mark-read"), FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (actions), mail_paned_view_selection_button (self,
		"mail-unread-symbolic", _("Unread"), _("Mark the selected messages as unread"), "mail-mark-unread"), FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (actions), mail_paned_view_selection_button (self,
		"oma-mail-flag-symbolic", NULL, _("Flag or unflag the selected messages"), "oma-mail-flag-toggle"), FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (actions), mail_paned_view_selection_button (self,
		"folder-symbolic", NULL, _("Move the selected messages to another folder"), "mail-move"), FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (actions), mail_paned_view_selection_button (self,
		"mail-archive", NULL, _("Archive the selected messages"), "mail-archive"), FALSE, FALSE, 0);

	widget = mail_paned_view_selection_button (self,
		"user-trash-symbolic", NULL, _("Delete the selected messages"), "mail-delete");
	gtk_style_context_add_class (gtk_widget_get_style_context (widget), "destructive-action");
	gtk_box_pack_start (GTK_BOX (actions), widget, FALSE, FALSE, 0);

	widget = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
	gtk_widget_set_margin_start (widget, 4);
	gtk_widget_set_margin_end (widget, 4);
	gtk_box_pack_start (GTK_BOX (bar), widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);

	widget = mail_paned_view_selection_button (self, "window-close-symbolic", NULL, _("Done selecting"), NULL);
	g_signal_connect (widget, "clicked", G_CALLBACK (mail_paned_view_selection_done_clicked_cb), self);
	gtk_box_pack_start (GTK_BOX (bar), widget, FALSE, FALSE, 0);

	g_signal_connect_object (self->priv->message_list, "selection-change",
		G_CALLBACK (mail_paned_view_selection_change_cb), self, 0);

	mail_paned_view_update_selection_bar (self);
}

/* —— Omarchy per-row quick actions ——
 * Hovering a message shows a small group of buttons on that row: toggle
 * read, toggle flag, archive and delete. They act on the hovered message
 * only, without changing the selection, so the message is not opened (and
 * therefore not marked as read) by using them. */

static GtkWidget *
mail_paned_view_get_list_canvas (EMailPanedView *self)
{
	ETableItem *item;

	if (!self->priv->message_list)
		return NULL;

	item = e_tree_get_item (E_TREE (self->priv->message_list));
	if (!item)
		return NULL;

	return GTK_WIDGET (GNOME_CANVAS_ITEM (item)->canvas);
}

static void
mail_paned_view_set_hover_row (EMailPanedView *self,
			       gint row)
{
	ETableItem *item;
	GtkWidget *canvas;

	if (!self->priv->message_list)
		return;

	item = e_tree_get_item (E_TREE (self->priv->message_list));
	if (!item || GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "oma-hover-row")) == row + 1)
		return;

	g_object_set_data (G_OBJECT (item), "oma-hover-row", GINT_TO_POINTER (row + 1));

	canvas = mail_paned_view_get_list_canvas (self);
	if (canvas)
		gtk_widget_queue_draw (canvas);
}

static void
mail_paned_view_row_actions_hide (EMailPanedView *self)
{
	if (self->priv->row_hide_id) {
		g_source_remove (self->priv->row_hide_id);
		self->priv->row_hide_id = 0;
	}

	if (self->priv->row_actions)
		gtk_widget_hide (self->priv->row_actions);

	mail_paned_view_set_hover_row (self, -1);
	g_clear_pointer (&self->priv->row_uid, g_free);
	self->priv->row_index = -1;
	self->priv->row_pointer_inside = FALSE;
}

static gboolean
mail_paned_view_row_actions_hide_cb (gpointer user_data)
{
	EMailPanedView *self = user_data;

	self->priv->row_hide_id = 0;

	if (!self->priv->row_pointer_inside)
		mail_paned_view_row_actions_hide (self);

	return G_SOURCE_REMOVE;
}

static void
mail_paned_view_row_actions_schedule_hide (EMailPanedView *self)
{
	if (!self->priv->row_hide_id)
		self->priv->row_hide_id = e_named_timeout_add (200, mail_paned_view_row_actions_hide_cb, self);
}

/* Shows the current read/flag state of the hovered message on the buttons. */
static void
mail_paned_view_row_actions_sync (EMailPanedView *self)
{
	CamelFolder *folder;
	CamelMessageInfo *info = NULL;
	guint32 flags = 0;
	gboolean seen, flagged;

	folder = message_list_ref_folder (MESSAGE_LIST (self->priv->message_list));
	if (folder && self->priv->row_uid)
		info = camel_folder_get_message_info (folder, self->priv->row_uid);
	if (info) {
		flags = camel_message_info_get_flags (info);
		g_object_unref (info);
	}
	g_clear_object (&folder);

	seen = (flags & CAMEL_MESSAGE_SEEN) != 0;
	flagged = (flags & CAMEL_MESSAGE_FLAGGED) != 0;

	gtk_image_set_from_icon_name (GTK_IMAGE (self->priv->row_read_image),
		seen ? "mail-unread-symbolic" : "mail-read-symbolic", GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text (self->priv->row_read_button,
		seen ? _("Mark as unread") : _("Mark as read"));

	gtk_widget_set_tooltip_text (self->priv->row_flag_button,
		flagged ? _("Remove flag") : _("Flag"));
	if (flagged)
		gtk_style_context_add_class (gtk_widget_get_style_context (self->priv->row_flag_button), "oma-flagged");
	else
		gtk_style_context_remove_class (gtk_widget_get_style_context (self->priv->row_flag_button), "oma-flagged");
}

static void
mail_paned_view_row_actions_update (EMailPanedView *self)
{
	GtkWidget *canvas;
	GdkWindow *window;
	GdkDevice *pointer;
	gint px = -1, py = -1, row = -1, col = -1;
	gint cx = 0, cy = 0, cw = 0, ch = 0, ox = 0, oy = 0;
	gchar *uid;

	canvas = mail_paned_view_get_list_canvas (self);
	if (!canvas || !self->priv->row_actions || !gtk_widget_get_mapped (canvas))
		return;

	window = gtk_widget_get_window (canvas);
	pointer = gdk_seat_get_pointer (gdk_display_get_default_seat (gtk_widget_get_display (canvas)));
	if (!window || !pointer)
		return;

	gdk_window_get_device_position (window, pointer, &px, &py, NULL);

	if (px < 0 || py < 0 || px >= gtk_widget_get_allocated_width (canvas) || py >= gtk_widget_get_allocated_height (canvas)) {
		mail_paned_view_row_actions_schedule_hide (self);
		return;
	}

	e_tree_get_cell_at (E_TREE (self->priv->message_list), px, py, &row, &col);
	uid = row >= 0 ? message_list_dup_uid_at_row (MESSAGE_LIST (self->priv->message_list), row) : NULL;

	if (!uid) {
		mail_paned_view_row_actions_schedule_hide (self);
		return;
	}

	if (self->priv->row_hide_id) {
		g_source_remove (self->priv->row_hide_id);
		self->priv->row_hide_id = 0;
	}

	if (row == self->priv->row_index && g_strcmp0 (uid, self->priv->row_uid) == 0 &&
	    gtk_widget_get_visible (self->priv->row_actions)) {
		g_free (uid);
		return;
	}

	g_free (self->priv->row_uid);
	self->priv->row_uid = uid;
	self->priv->row_index = row;

	{
		ETableItem *item = e_tree_get_item (E_TREE (self->priv->message_list));
		gint vrow = row, vcol = 0, sx = 0, sy = 0;
		gdouble wx, wy;

		/* Item coordinates -> canvas pixels -> visible widget area. */
		e_table_item_get_cell_geometry (item, &vrow, &vcol, &cx, &cy, &cw, &ch);
		wx = cx;
		wy = cy;
		gnome_canvas_item_i2w (GNOME_CANVAS_ITEM (item), &wx, &wy);
		gnome_canvas_w2c (GNOME_CANVAS (canvas), wx, wy, &cx, &cy);
		gnome_canvas_get_scroll_offsets (GNOME_CANVAS (canvas), &sx, &sy);
		cx -= sx;
		cy -= sy;
	}

	gtk_widget_translate_coordinates (canvas, self->priv->list_overlay, cx, cy, &ox, &oy);

	/* The same inset as the painted row card (see eti_draw). */
	self->priv->row_rect.x = ox + 8;
	self->priv->row_rect.y = oy + 3;
	self->priv->row_rect.width = cw - 16;
	self->priv->row_rect.height = ch - 7;

	mail_paned_view_set_hover_row (self, row);

	if (e_selection_model_is_row_selected (e_tree_get_selection_model (E_TREE (self->priv->message_list)), row))
		gtk_style_context_add_class (gtk_widget_get_style_context (self->priv->row_actions), "oma-on-selected");
	else
		gtk_style_context_remove_class (gtk_widget_get_style_context (self->priv->row_actions), "oma-on-selected");

	mail_paned_view_row_actions_sync (self);
	gtk_widget_show (self->priv->row_actions);
	gtk_widget_queue_resize (self->priv->row_actions);
}

static gboolean
mail_paned_view_overlay_get_child_position_cb (GtkOverlay *overlay,
					       GtkWidget *widget,
					       GdkRectangle *allocation,
					       gpointer user_data)
{
	EMailPanedView *self = user_data;
	gint width = 0, height = 0;

	if (widget != self->priv->row_actions)
		return FALSE;

	gtk_widget_get_preferred_width (widget, NULL, &width);
	gtk_widget_get_preferred_height (widget, NULL, &height);

	/* Top-right of the row card, on the sender line, where the time is:
	 * the actions take the time's place while the row is hovered. */
	allocation->width = width;
	allocation->height = height;
	allocation->x = MAX (self->priv->row_rect.x, self->priv->row_rect.x + self->priv->row_rect.width - width - 4);
	allocation->y = self->priv->row_rect.y + 6;

	return TRUE;
}

static gboolean
mail_paned_view_list_motion_cb (GtkWidget *widget,
				GdkEvent *event,
				gpointer user_data)
{
	mail_paned_view_row_actions_update (E_MAIL_PANED_VIEW (user_data));

	return FALSE;
}

static gboolean
mail_paned_view_list_leave_cb (GtkWidget *widget,
			       GdkEventCrossing *event,
			       gpointer user_data)
{
	mail_paned_view_row_actions_schedule_hide (E_MAIL_PANED_VIEW (user_data));

	return FALSE;
}

static gboolean
mail_paned_view_row_actions_crossing_cb (GtkWidget *widget,
					 GdkEventCrossing *event,
					 gpointer user_data)
{
	EMailPanedView *self = user_data;

	if (event->detail == GDK_NOTIFY_INFERIOR)
		return FALSE;

	self->priv->row_pointer_inside = event->type == GDK_ENTER_NOTIFY;

	if (self->priv->row_pointer_inside) {
		if (self->priv->row_hide_id) {
			g_source_remove (self->priv->row_hide_id);
			self->priv->row_hide_id = 0;
		}
	} else {
		mail_paned_view_row_actions_schedule_hide (self);
	}

	return FALSE;
}

static void
mail_paned_view_list_scrolled_cb (GtkAdjustment *adjustment,
				  gpointer user_data)
{
	/* Rows moved under the pointer; follow on the next motion. */
	mail_paned_view_row_actions_hide (E_MAIL_PANED_VIEW (user_data));
}

/* When the hovered message is the one open in the reader, use the regular
 * actions, so the reader's own logic (mark-as-read timer, moving to the
 * next message after delete) stays consistent. */
static gboolean
mail_paned_view_row_is_current (EMailPanedView *self)
{
	MessageList *message_list = MESSAGE_LIST (self->priv->message_list);

	return message_list_selected_count (message_list) == 1 &&
		g_strcmp0 (message_list->cursor_uid, self->priv->row_uid) == 0;
}

static void
mail_paned_view_row_run_action (EMailPanedView *self,
				const gchar *action_name)
{
	EUIAction *action;

	action = e_mail_reader_get_action (E_MAIL_READER (self), action_name);
	if (action && e_ui_action_get_sensitive (action))
		g_action_activate (G_ACTION (action), NULL);
}

static void
mail_paned_view_row_set_flags (EMailPanedView *self,
			       guint32 mask,
			       guint32 set)
{
	CamelFolder *folder;

	folder = message_list_ref_folder (MESSAGE_LIST (self->priv->message_list));
	if (folder && self->priv->row_uid)
		camel_folder_set_message_flags (folder, self->priv->row_uid, mask, set);
	g_clear_object (&folder);
}

static guint32
mail_paned_view_row_get_flags (EMailPanedView *self)
{
	CamelFolder *folder;
	CamelMessageInfo *info = NULL;
	guint32 flags = 0;

	folder = message_list_ref_folder (MESSAGE_LIST (self->priv->message_list));
	if (folder && self->priv->row_uid)
		info = camel_folder_get_message_info (folder, self->priv->row_uid);
	if (info) {
		flags = camel_message_info_get_flags (info);
		g_object_unref (info);
	}
	g_clear_object (&folder);

	return flags;
}

static void
mail_paned_view_row_read_clicked_cb (GtkButton *button,
				     gpointer user_data)
{
	EMailPanedView *self = user_data;
	gboolean seen = (mail_paned_view_row_get_flags (self) & CAMEL_MESSAGE_SEEN) != 0;

	if (mail_paned_view_row_is_current (self))
		mail_paned_view_row_run_action (self, seen ? "mail-mark-unread" : "mail-mark-read");
	else
		mail_paned_view_row_set_flags (self, CAMEL_MESSAGE_SEEN, seen ? 0 : CAMEL_MESSAGE_SEEN);

	mail_paned_view_row_actions_sync (self);
}

static void
mail_paned_view_row_flag_clicked_cb (GtkButton *button,
				     gpointer user_data)
{
	EMailPanedView *self = user_data;
	gboolean flagged = (mail_paned_view_row_get_flags (self) & CAMEL_MESSAGE_FLAGGED) != 0;

	mail_paned_view_row_set_flags (self, CAMEL_MESSAGE_FLAGGED, flagged ? 0 : CAMEL_MESSAGE_FLAGGED);
	mail_paned_view_row_actions_sync (self);
}

static void
mail_paned_view_row_delete_clicked_cb (GtkButton *button,
				       gpointer user_data)
{
	EMailPanedView *self = user_data;

	if (mail_paned_view_row_is_current (self)) {
		mail_paned_view_row_run_action (self, "mail-delete");
	} else {
		e_tree_preserve_viewport (E_TREE (self->priv->message_list));
		mail_paned_view_row_set_flags (self,
			CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_DELETED,
			CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_DELETED);
	}

	mail_paned_view_row_actions_hide (self);
}

static void
mail_paned_view_row_archive_clicked_cb (GtkButton *button,
					gpointer user_data)
{
	EMailPanedView *self = user_data;
	EMailBackend *backend;
	CamelFolder *folder;
	GPtrArray *uids;
	gchar *archive_folder;

	if (!self->priv->row_uid)
		return;

	if (mail_paned_view_row_is_current (self)) {
		mail_paned_view_row_run_action (self, "mail-archive");
		mail_paned_view_row_actions_hide (self);
		return;
	}

	backend = e_mail_reader_get_backend (E_MAIL_READER (self));
	folder = message_list_ref_folder (MESSAGE_LIST (self->priv->message_list));
	if (!folder)
		return;

	uids = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (uids, g_strdup (self->priv->row_uid));

	archive_folder = em_utils_get_archive_folder_uri_from_folder (folder, backend, uids, TRUE);

	if (archive_folder && *archive_folder) {
		e_tree_preserve_viewport (E_TREE (self->priv->message_list));
		mail_transfer_messages (
			e_mail_backend_get_session (backend), folder, uids,
			TRUE, archive_folder, 0, NULL, NULL);
	} else {
		e_alert_submit (e_mail_reader_get_alert_sink (E_MAIL_READER (self)),
			"mail:no-archive-folder", NULL);
	}

	g_free (archive_folder);
	g_ptr_array_unref (uids);
	g_object_unref (folder);

	mail_paned_view_row_actions_hide (self);
}

static GtkWidget *
mail_paned_view_row_button (EMailPanedView *self,
			    const gchar *icon_name,
			    const gchar *tooltip,
			    GCallback callback,
			    GtkWidget **out_image)
{
	GtkWidget *button, *image;

	image = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_BUTTON);
	gtk_widget_show (image);

	button = gtk_button_new ();
	gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
	gtk_widget_set_can_focus (button, FALSE);
	gtk_widget_set_tooltip_text (button, tooltip);
	gtk_container_add (GTK_CONTAINER (button), image);
	g_signal_connect (button, "clicked", callback, self);
	gtk_widget_show (button);

	if (out_image)
		*out_image = image;

	return button;
}

static void
mail_paned_view_build_row_actions (EMailPanedView *self)
{
	GtkWidget *event_box, *box, *widget, *canvas;

	self->priv->row_index = -1;

	event_box = gtk_event_box_new ();
	gtk_widget_add_events (event_box, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
	gtk_widget_set_no_show_all (event_box, TRUE);
	g_signal_connect (event_box, "enter-notify-event",
		G_CALLBACK (mail_paned_view_row_actions_crossing_cb), self);
	g_signal_connect (event_box, "leave-notify-event",
		G_CALLBACK (mail_paned_view_row_actions_crossing_cb), self);
	gtk_overlay_add_overlay (GTK_OVERLAY (self->priv->list_overlay), event_box);
	self->priv->row_actions = event_box;

	box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_name (box, "omamail-row-actions");
	gtk_container_add (GTK_CONTAINER (event_box), box);
	gtk_widget_show (box);

	self->priv->row_read_button = mail_paned_view_row_button (self, "mail-read-symbolic", _("Mark as read"),
		G_CALLBACK (mail_paned_view_row_read_clicked_cb), &self->priv->row_read_image);
	gtk_box_pack_start (GTK_BOX (box), self->priv->row_read_button, FALSE, FALSE, 0);

	self->priv->row_flag_button = mail_paned_view_row_button (self, "oma-mail-flag-symbolic", _("Flag"),
		G_CALLBACK (mail_paned_view_row_flag_clicked_cb), NULL);
	gtk_box_pack_start (GTK_BOX (box), self->priv->row_flag_button, FALSE, FALSE, 0);

	widget = mail_paned_view_row_button (self, "mail-archive", _("Archive"),
		G_CALLBACK (mail_paned_view_row_archive_clicked_cb), NULL);
	gtk_box_pack_start (GTK_BOX (box), widget, FALSE, FALSE, 0);

	widget = mail_paned_view_row_button (self, "user-trash-symbolic", _("Delete"),
		G_CALLBACK (mail_paned_view_row_delete_clicked_cb), NULL);
	gtk_style_context_add_class (gtk_widget_get_style_context (widget), "destructive-action");
	gtk_box_pack_start (GTK_BOX (box), widget, FALSE, FALSE, 0);

	g_signal_connect_object (self->priv->list_overlay, "get-child-position",
		G_CALLBACK (mail_paned_view_overlay_get_child_position_cb), self, 0);

	canvas = mail_paned_view_get_list_canvas (self);
	if (canvas) {
		gtk_widget_add_events (canvas, GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
		g_signal_connect_object (canvas, "motion-notify-event",
			G_CALLBACK (mail_paned_view_list_motion_cb), self, 0);
		g_signal_connect_object (canvas, "leave-notify-event",
			G_CALLBACK (mail_paned_view_list_leave_cb), self, 0);
	}

	if (self->priv->scrolled_window) {
		GtkAdjustment *adjustment;

		adjustment = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->priv->scrolled_window));
		g_signal_connect_object (adjustment, "value-changed",
			G_CALLBACK (mail_paned_view_list_scrolled_cb), self, 0);
	}

	/* The list was rebuilt (folder switch, search, deletion) */
	g_signal_connect_object (self->priv->message_list, "message-list-built",
		G_CALLBACK (mail_paned_view_row_actions_hide), self, G_CONNECT_SWAPPED);
}

static void
mail_paned_display_view_cb (GalViewInstance *view_instance,
                            GalView *gal_view,
                            EMailView *view)
{
	EMailReader *reader;
	EShellView *shell_view;
	GtkWidget *message_list;

	shell_view = e_mail_view_get_shell_view (view);
	e_shell_view_set_view_instance (shell_view, view_instance);

	reader = E_MAIL_READER (view);
	message_list = e_mail_reader_get_message_list (reader);

	if (GAL_IS_VIEW_ETABLE (gal_view))
		gal_view_etable_attach_tree (
			GAL_VIEW_ETABLE (gal_view),
			E_TREE (message_list));
}

static void
mail_paned_view_notify_group_by_threads_cb (EMailReader *reader)
{
	gboolean group_by_threads;

	group_by_threads = e_mail_reader_get_group_by_threads (reader);

	mail_paned_view_save_boolean (
		E_MAIL_VIEW (reader),
		STATE_KEY_GROUP_BY_THREADS, group_by_threads);
}

static void
mail_paned_view_set_property (GObject *object,
                              guint property_id,
                              const GValue *value,
                              GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_FORWARD_STYLE:
			e_mail_reader_set_forward_style (
				E_MAIL_READER (object),
				g_value_get_enum (value));
			return;

		case PROP_GROUP_BY_THREADS:
			e_mail_reader_set_group_by_threads (
				E_MAIL_READER (object),
				g_value_get_boolean (value));
			return;

		case PROP_REPLY_STYLE:
			e_mail_reader_set_reply_style (
				E_MAIL_READER (object),
				g_value_get_enum (value));
			return;

		case PROP_MARK_SEEN_ALWAYS:
			e_mail_reader_set_mark_seen_always (
				E_MAIL_READER (object),
				g_value_get_boolean (value));
			return;

		case PROP_DELETE_SELECTS_PREVIOUS:
			e_mail_reader_set_delete_selects_previous (
				E_MAIL_READER (object),
				g_value_get_boolean (value));
			return;

		case PROP_PREVIEW_TOOLBAR_VISIBLE:
			e_mail_paned_view_set_preview_toolbar_visible (
				E_MAIL_PANED_VIEW (object),
				g_value_get_boolean (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
mail_paned_view_get_property (GObject *object,
                              guint property_id,
                              GValue *value,
                              GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_FORWARD_STYLE:
			g_value_set_enum (
				value,
				e_mail_reader_get_forward_style (
				E_MAIL_READER (object)));
			return;

		case PROP_GROUP_BY_THREADS:
			g_value_set_boolean (
				value,
				e_mail_reader_get_group_by_threads (
				E_MAIL_READER (object)));
			return;

		case PROP_REPLY_STYLE:
			g_value_set_enum (
				value,
				e_mail_reader_get_reply_style (
				E_MAIL_READER (object)));
			return;

		case PROP_MARK_SEEN_ALWAYS:
			g_value_set_boolean (
				value,
				e_mail_reader_get_mark_seen_always (
				E_MAIL_READER (object)));
			return;

		case PROP_DELETE_SELECTS_PREVIOUS:
			g_value_set_boolean (
				value,
				e_mail_reader_get_delete_selects_previous (
				E_MAIL_READER (object)));
			return;

		case PROP_PREVIEW_TOOLBAR_VISIBLE:
			g_value_set_boolean (
				value,
				e_mail_paned_view_get_preview_toolbar_visible (
				E_MAIL_PANED_VIEW (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
mail_paned_view_dispose (GObject *object)
{
	EMailPanedView *self = E_MAIL_PANED_VIEW (object);

	if (self->priv->responsive_idle) {
		g_source_remove (self->priv->responsive_idle);
		self->priv->responsive_idle = 0;
	}
	e_mail_reader_dispose (E_MAIL_READER (object));

	g_clear_object (&self->priv->paned);
	g_clear_object (&self->priv->scrolled_window);
	g_clear_object (&self->priv->list_column);
	if (self->priv->row_hide_id) {
		g_source_remove (self->priv->row_hide_id);
		self->priv->row_hide_id = 0;
	}
	g_clear_pointer (&self->priv->row_uid, g_free);
	self->priv->row_actions = NULL;
	g_clear_object (&self->priv->list_overlay);
	self->priv->selection_revealer = NULL;
	self->priv->selection_label = NULL;
	self->priv->selection_actions = NULL;

	if (self->priv->message_list != NULL) {
		/* It can be disconnected by EMailReader in e_mail_reader_dispose() */
		if (self->priv->message_list_built_id &&
		    g_signal_handler_is_connected (self->priv->message_list, self->priv->message_list_built_id)) {
			g_signal_handler_disconnect (self->priv->message_list, self->priv->message_list_built_id);
		}

		self->priv->message_list_built_id = 0;

		g_clear_object (&self->priv->message_list);
	}

	g_clear_object (&self->priv->preview_pane);
	g_clear_object (&self->priv->preview_toolbar_box);
	g_clear_object (&self->priv->view_instance);

	g_clear_pointer (&self->priv->last_selected_uid, g_free);

	self->priv->display = NULL;

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_mail_paned_view_parent_class)->dispose (object);
}

static EAlertSink *
mail_paned_view_get_alert_sink (EMailReader *reader)
{
	EMailView *view;
	EShellView *shell_view;
	EShellContent *shell_content;

	view = E_MAIL_VIEW (reader);
	shell_view = e_mail_view_get_shell_view (view);
	shell_content = e_shell_view_get_shell_content (shell_view);

	return E_ALERT_SINK (shell_content);
}

static EMailBackend *
mail_paned_view_get_backend (EMailReader *reader)
{
	EMailView *view;
	EShellView *shell_view;
	EShellBackend *shell_backend;

	view = E_MAIL_VIEW (reader);
	shell_view = e_mail_view_get_shell_view (view);
	shell_backend = e_shell_view_get_shell_backend (shell_view);

	return E_MAIL_BACKEND (shell_backend);
}

static EMailDisplay *
mail_paned_view_get_mail_display (EMailReader *reader)
{
	EMailPanedViewPrivate *priv;

	priv = E_MAIL_PANED_VIEW (reader)->priv;

	return priv->display;
}

static gboolean
mail_paned_view_get_hide_deleted (EMailReader *reader)
{
	return !e_mail_view_get_show_deleted (E_MAIL_VIEW (reader));
}

static GtkWidget *
mail_paned_view_get_message_list (EMailReader *reader)
{
	EMailPanedView *paned_view;

	paned_view = E_MAIL_PANED_VIEW (reader);

	return paned_view->priv->message_list;
}

static EUIManager *
mail_paned_view_get_ui_manager (EMailReader *reader)
{
	EShellView *shell_view;

	shell_view = e_mail_view_get_shell_view (E_MAIL_VIEW (reader));

	return e_shell_view_get_ui_manager (shell_view);
}

static EPreviewPane *
mail_paned_view_get_preview_pane (EMailReader *reader)
{
	EMailPanedView *paned_view;

	paned_view = E_MAIL_PANED_VIEW (reader);

	return E_PREVIEW_PANE (paned_view->priv->preview_pane);
}

static GtkWindow *
mail_paned_view_get_window (EMailReader *reader)
{
	EMailView *view;
	EShellView *shell_view;
	EShellWindow *shell_window;

	view = E_MAIL_VIEW (reader);
	shell_view = e_mail_view_get_shell_view (view);
	shell_window = e_shell_view_get_shell_window (shell_view);

	return GTK_WINDOW (shell_window);
}

static void
mail_paned_view_set_folder (EMailReader *reader,
                            CamelFolder *folder)
{
	EMailPanedView *self = E_MAIL_PANED_VIEW (reader);
	EMailView *view;
	EShell *shell;
	EShellView *shell_view;
	EShellWindow *shell_window;
	GSettings *settings;
	EMailReaderInterface *default_interface;
	GtkWidget *message_list;
	GKeyFile *key_file;
	CamelFolder *previous_folder;
	gchar *folder_uri;
	gchar *group_name;
	const gchar *key;
	gboolean value, global_view_setting;
	GError *error = NULL;

	view = E_MAIL_VIEW (reader);
	shell_view = e_mail_view_get_shell_view (view);

	/* Can be NULL, if the shell window was closed meanwhile */
	if (!shell_view)
		return;

	previous_folder = e_mail_reader_ref_folder (reader);
	if (previous_folder == folder) {
		g_clear_object (&previous_folder);
		return;
	}

	if (self->priv->last_selected_uid && previous_folder && folder &&
	    CAMEL_IS_VEE_FOLDER (previous_folder)) {
		CamelFolder *real_folder = NULL;
		gchar *message_uid = NULL;

		em_utils_get_real_folder_and_message_uid (previous_folder, self->priv->last_selected_uid, &real_folder, NULL, &message_uid);

		g_clear_pointer (&self->priv->last_selected_uid, g_free);

		if (real_folder == folder && message_uid) {
			self->priv->last_selected_uid = message_uid;
			message_uid = NULL;
		}

		g_free (message_uid);
		g_clear_object (&real_folder);
	} else {
		g_clear_pointer (&self->priv->last_selected_uid, g_free);
	}

	g_clear_object (&previous_folder);

	shell_window = e_shell_view_get_shell_window (shell_view);

	shell = e_shell_window_get_shell (shell_window);

	settings = e_util_ref_settings ("org.gnome.evolution.mail");

	/* FIXME This should be an EMailReader property. */
	global_view_setting = g_settings_get_boolean (
		settings, "global-view-setting");

	message_list = e_mail_reader_get_message_list (reader);

	message_list_freeze (MESSAGE_LIST (message_list));

	/* Chain up to interface's default set_folder() method. */
	default_interface = g_type_default_interface_peek (E_TYPE_MAIL_READER);
	default_interface->set_folder (reader, folder);

	mail_paned_view_update_list_title (self, folder);

	if (folder == NULL)
		goto exit;

	/* Only refresh the folder if we're online. */
	if (e_shell_get_online (shell))
		e_mail_reader_refresh_folder (reader, folder);

	self->priv->folder_just_set = TRUE;

	/* Restore the folder's preview and threaded state. */

	folder_uri = e_mail_folder_uri_from_folder (folder);
	key_file = e_shell_view_get_state_key_file (shell_view);
	group_name = g_strdup_printf ("Folder %s", folder_uri);
	g_free (folder_uri);

	key = STATE_KEY_GROUP_BY_THREADS;
	value = g_key_file_get_boolean (key_file, global_view_setting ? STATE_GROUP_GLOBAL_FOLDER : group_name, key, &error);
	if (error != NULL) {
		g_clear_error (&error);

		value = !global_view_setting ||
			g_key_file_get_boolean (key_file, STATE_GROUP_GLOBAL_FOLDER, key, &error);
		if (error != NULL) {
			g_clear_error (&error);
			value = TRUE;
		}
	}

	e_mail_reader_set_group_by_threads (reader, value);

	key = STATE_KEY_PREVIEW_VISIBLE;
	value = g_key_file_get_boolean (key_file, global_view_setting ? STATE_GROUP_GLOBAL_FOLDER : group_name, key, &error);
	if (error != NULL) {
		g_clear_error (&error);

		value = !global_view_setting ||
			g_key_file_get_boolean (key_file, STATE_GROUP_GLOBAL_FOLDER, key, &error);
		if (error != NULL) {
			g_clear_error (&error);
			value = TRUE;
		}
	}

	/* XXX This is a little confusing and needs rethought.  The
	 *     EShellWindow:safe-mode property blocks automatic message
	 *     selection, but the "safe-list" setting blocks both the
	 *     preview pane and automatic message selection. */
	if (g_settings_get_boolean (settings, "safe-list")) {
		g_settings_set_boolean (settings, "safe-list", FALSE);
		e_shell_window_set_safe_mode (shell_window, TRUE);
		value = FALSE;
	}

	e_mail_view_set_preview_visible (E_MAIL_VIEW (reader), value);

	g_free (group_name);

exit:
	message_list_thaw (MESSAGE_LIST (message_list));

	g_object_unref (settings);
}

static guint
mail_paned_view_reader_open_selected_mail (EMailReader *reader)
{
	EMailPanedView *paned_view;
	EMailPanedViewClass *class;

	paned_view = E_MAIL_PANED_VIEW (reader);

	class = E_MAIL_PANED_VIEW_GET_CLASS (paned_view);
	g_return_val_if_fail (class->open_selected_mail != NULL, 0);

	return class->open_selected_mail (paned_view);
}

/* —— List column header ——
 * A large mailbox title with a quieter accent scope beside it
 * ("Inbox  all accounts."), then a row for the filter chips, which the
 * mail module supplies through e_mail_paned_view_take_list_filters(). */
static void
mail_paned_view_build_list_column (EMailPanedView *self)
{
	GtkWidget *header, *row, *widget;

	widget = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_name (widget, "omamail-list-column");
	self->priv->list_column = g_object_ref (widget);
	gtk_widget_show (widget);

	header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_name (header, "omamail-list-header");
	gtk_box_pack_start (GTK_BOX (self->priv->list_column), header, FALSE, FALSE, 0);
	gtk_widget_show (header);

	row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_pack_start (GTK_BOX (header), row, FALSE, FALSE, 0);
	gtk_widget_show (row);

	widget = gtk_label_new ("");
	gtk_label_set_xalign (GTK_LABEL (widget), 0.0);
	gtk_label_set_ellipsize (GTK_LABEL (widget), PANGO_ELLIPSIZE_END);
	gtk_style_context_add_class (gtk_widget_get_style_context (widget), "oma-list-title");
	gtk_box_pack_start (GTK_BOX (row), widget, FALSE, FALSE, 0);
	self->priv->list_title = widget;
	gtk_widget_show (widget);

	widget = gtk_label_new ("");
	gtk_label_set_xalign (GTK_LABEL (widget), 0.0);
	gtk_widget_set_valign (widget, GTK_ALIGN_BASELINE);
	gtk_label_set_ellipsize (GTK_LABEL (widget), PANGO_ELLIPSIZE_END);
	gtk_style_context_add_class (gtk_widget_get_style_context (widget), "oma-list-subtitle");
	gtk_box_pack_start (GTK_BOX (row), widget, TRUE, TRUE, 0);
	self->priv->list_subtitle = widget;

	gtk_widget_set_valign (self->priv->list_title, GTK_ALIGN_BASELINE);

	widget = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_name (widget, "omamail-list-filters");
	gtk_box_pack_start (GTK_BOX (header), widget, FALSE, FALSE, 0);
	self->priv->list_filters_box = widget;
	gtk_widget_set_no_show_all (widget, TRUE);
}

static void
mail_paned_view_update_list_title (EMailPanedView *self,
				   CamelFolder *folder)
{
	const gchar *title = NULL;
	gchar *subtitle = NULL;

	if (!self->priv->list_title)
		return;

	if (folder) {
		const gchar *name = camel_folder_get_display_name (folder);

		if (CAMEL_IS_VEE_FOLDER (folder)) {
			/* Unified mailboxes read as the mailbox, across accounts. */
			if (g_strcmp0 (name, "All Inboxes") == 0)
				title = _("Inbox");
			else if (g_strcmp0 (name, "All Sent") == 0)
				title = _("Sent");
			else
				title = name;
			subtitle = g_strdup (_("all accounts."));
		} else {
			CamelStore *store = camel_folder_get_parent_store (folder);

			title = name;
			if (store)
				subtitle = g_strdup (camel_service_get_display_name (CAMEL_SERVICE (store)));
		}
	}

	gtk_label_set_text (GTK_LABEL (self->priv->list_title), title ? title : "");
	gtk_label_set_text (GTK_LABEL (self->priv->list_subtitle), subtitle ? subtitle : "");
	gtk_widget_set_visible (self->priv->list_subtitle, subtitle && *subtitle);

	g_free (subtitle);
}

static void
mail_paned_view_constructed (GObject *object)
{
	EMailPanedView *self = E_MAIL_PANED_VIEW (object);
	EShellBackend *shell_backend;
	EShellWindow *shell_window;
	EShellView *shell_view;
	GSettings *settings;
	EMailReader *reader;
	EMailBackend *backend;
	EMailSession *session;
	EMailView *view;
	GtkWidget *message_list;
	GtkWidget *container;
	GtkWidget *widget;

	view = E_MAIL_VIEW (object);
	shell_view = e_mail_view_get_shell_view (view);
	shell_window = e_shell_view_get_shell_window (shell_view);
	shell_backend = e_shell_view_get_shell_backend (shell_view);

	backend = E_MAIL_BACKEND (shell_backend);
	session = e_mail_backend_get_session (backend);

	self->priv->display = g_object_new (E_TYPE_MAIL_DISPLAY,
		"headers-collapsable", TRUE,
		"remote-content", e_mail_backend_get_remote_content (backend),
		"mail-reader", E_MAIL_READER (self),
		NULL);

	/* FIXME This should be an EMailPanedView property, so
	 *       it can be configured from the settings module. */

	settings = e_util_ref_settings ("org.gnome.evolution.mail");

	g_settings_bind (
		settings, "headers-collapsed",
		self->priv->display, "headers-collapsed",
		G_SETTINGS_BIND_DEFAULT);

	g_object_unref (settings);

	/* Build content widgets. */

	container = GTK_WIDGET (object);

	/* The quick selection bar floats over the whole view, so it fits
	 * even when the message list column is narrow. */
	widget = gtk_overlay_new ();
	gtk_box_pack_start (GTK_BOX (container), widget, TRUE, TRUE, 0);
	self->priv->list_overlay = g_object_ref (widget);
	gtk_widget_show (widget);

	container = widget;

	widget = e_paned_new (GTK_ORIENTATION_VERTICAL);
	e_paned_set_fixed_resize (E_PANED (widget), FALSE);
	gtk_container_add (GTK_CONTAINER (container), widget);
	self->priv->paned = g_object_ref (widget);
	gtk_widget_show (widget);

	e_binding_bind_property (
		object, "orientation",
		widget, "orientation",
		G_BINDING_SYNC_CREATE);

	container = self->priv->paned;

	mail_paned_view_build_list_column (self);
	gtk_paned_pack1 (GTK_PANED (container), self->priv->list_column, TRUE, TRUE);

	widget = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (
		GTK_SCROLLED_WINDOW (widget),
		GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	gtk_box_pack_start (GTK_BOX (self->priv->list_column), widget, TRUE, TRUE, 0);
	self->priv->scrolled_window = g_object_ref (widget);
	gtk_widget_show (widget);

	container = widget;

	widget = message_list_new (session);
	gtk_container_add (GTK_CONTAINER (container), widget);
	self->priv->message_list = g_object_ref (widget);
	gtk_widget_show (widget);

	self->priv->message_list_built_id = g_signal_connect_swapped (
		self->priv->message_list, "message-list-built",
		G_CALLBACK (mail_paned_view_message_list_built_cb),
		object);

	mail_paned_view_build_selection_bar (self);
	mail_paned_view_build_row_actions (self);

	container = self->priv->paned;

	widget = GTK_WIDGET (e_mail_display_get_attachment_view (self->priv->display));
	gtk_widget_show (widget);
	gtk_paned_pack2 (GTK_PANED (container), widget, FALSE, TRUE);

	e_binding_bind_property (
		object, "preview-visible",
		widget, "visible",
		G_BINDING_SYNC_CREATE);

	container = e_attachment_bar_get_content_area (E_ATTACHMENT_BAR (widget));
	widget = e_preview_pane_new (E_WEB_VIEW (self->priv->display));

	/* cannot ask the shell_view for the "mail-preview-toolbar" now, because its EUIManager is not loaded yet;
	   postpone it to the e_mail_paned_view_take_preview_toolbar() function */
	self->priv->preview_toolbar_box = g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
	gtk_box_pack_start (GTK_BOX (container), GTK_WIDGET (self->priv->preview_toolbar_box), FALSE, FALSE, 0);

	gtk_box_pack_start (GTK_BOX (container), widget, TRUE, TRUE, 0);

	self->priv->preview_pane = g_object_ref (widget);
	gtk_widget_show (GTK_WIDGET (self->priv->display));
	gtk_widget_show (widget);

	/* Load the view instance. */

	e_mail_view_update_view_instance (E_MAIL_VIEW (object));

	/* Message list customizations. */

	e_mail_reader_init (E_MAIL_READER (object));

	reader = E_MAIL_READER (object);
	message_list = e_mail_reader_get_message_list (reader);

	g_signal_connect_swapped (
		message_list, "message-selected",
		G_CALLBACK (mail_paned_view_message_selected_cb),
		object);

	/* Restore pane positions from the last session once
	 * the shell view is fully initialized and visible. */
	g_signal_connect (
		shell_window, "shell-view-created::mail",
		G_CALLBACK (mail_paned_view_restore_state_cb),
		object);

	/* Do this after creating the message list.  Our
	 * set_preview_visible() method relies on it. */
	e_mail_view_set_preview_visible (view, TRUE);

	e_extensible_load_extensions (E_EXTENSIBLE (object));

	g_signal_connect (self, "size-allocate",
		G_CALLBACK (mail_paned_view_responsive_size_allocate_cb), NULL);

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (e_mail_paned_view_parent_class)->constructed (object);
}

static void
mail_paned_view_set_search_strings (EMailView *view,
                                    GSList *search_strings)
{
	EMailDisplay *display;
	EWebView *web_view;
	EMailReader *reader;

	reader = E_MAIL_READER (view);
	display = e_mail_reader_get_mail_display (reader);
	if (!display)
		return;

	web_view = E_WEB_VIEW (display);

	e_web_view_clear_highlights (web_view);

	while (search_strings != NULL) {
		e_web_view_add_highlight (web_view, search_strings->data);
		search_strings = g_slist_next (search_strings);
	}
}

static GalViewInstance *
mail_paned_view_get_view_instance (EMailView *view)
{
	EMailPanedView *paned_view;

	paned_view = E_MAIL_PANED_VIEW (view);

	return paned_view->priv->view_instance;
}

static gchar *
empv_create_view_id (CamelFolder *folder)
{
	gchar *res, *folder_uri;

	g_return_val_if_fail (folder != NULL, NULL);

	folder_uri = e_mail_folder_uri_from_folder (folder);
	g_return_val_if_fail (folder_uri != NULL, NULL);

	res = mail_config_folder_uri_to_view_id (folder_uri);

	g_free (folder_uri);

	return res;
}

static gboolean
empv_folder_or_parent_is_outgoing (MailFolderCache *folder_cache,
				   CamelStore *store,
				   const gchar *fullname)
{
	CamelFolderInfoFlags info_flags;
	gchar *path, *dash;
	gboolean res = FALSE;

	g_return_val_if_fail (MAIL_IS_FOLDER_CACHE (folder_cache), FALSE);
	g_return_val_if_fail (CAMEL_IS_STORE (store), FALSE);
	g_return_val_if_fail (fullname != NULL, FALSE);

	if (!mail_folder_cache_get_folder_info_flags (folder_cache, store, fullname, &info_flags))
		info_flags = 0;

	if ((info_flags & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_OUTBOX ||
	    (info_flags & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_SENT)
		return TRUE;

	if (!strchr (fullname, '/'))
		return FALSE;

	path = g_strdup (fullname);

	while (path && *path) {
		dash = strrchr (path, '/');
		if (!dash)
			break;

		*dash = '\0';

		if (!mail_folder_cache_get_folder_info_flags (folder_cache, store, path, &info_flags))
			continue;

		if ((info_flags & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_OUTBOX ||
		    (info_flags & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_SENT) {
			res = TRUE;
			break;
		}
	}

	g_free (path);

	return res;
}

static void
mail_paned_view_update_view_instance (EMailView *view)
{
	EMailPanedView *self = E_MAIL_PANED_VIEW (view);
	EMailReader *reader;
	EShell *shell;
	EShellView *shell_view;
	EShellWindow *shell_window;
	EShellViewClass *shell_view_class;
	ESourceRegistry *registry;
	GalViewCollection *view_collection;
	GalViewInstance *view_instance;
	MailFolderCache *folder_cache;
	CamelFolder *folder;
	GtkOrientable *orientable;
	GtkOrientation orientation;
	GSettings *settings;
	gboolean outgoing_folder;
	gboolean show_vertical_view;
	gboolean global_view_setting;
	gchar *view_id;

	shell_view = e_mail_view_get_shell_view (view);
	shell_view_class = E_SHELL_VIEW_GET_CLASS (shell_view);
	view_collection = shell_view_class->view_collection;

	shell_window = e_shell_view_get_shell_window (shell_view);
	shell = e_shell_window_get_shell (shell_window);
	registry = e_shell_get_registry (shell);

	reader = E_MAIL_READER (view);
	folder = e_mail_reader_ref_folder (reader);

	/* If no folder is selected, return silently. */
	if (folder == NULL)
		return;

	g_clear_object (&self->priv->view_instance);

	view_id = empv_create_view_id (folder);
	e_util_make_safe_filename (view_id);

	folder_cache = e_mail_session_get_folder_cache (e_mail_backend_get_session (e_mail_reader_get_backend (reader)));

	outgoing_folder =
		empv_folder_or_parent_is_outgoing (folder_cache, camel_folder_get_parent_store (folder), camel_folder_get_full_name (folder)) ||
		em_utils_folder_is_drafts (registry, folder) ||
		em_utils_folder_is_outbox (registry, folder) ||
		em_utils_folder_is_sent (registry, folder);

	settings = e_util_ref_settings ("org.gnome.evolution.mail");
	global_view_setting = g_settings_get_boolean (
		settings, "global-view-setting");
	g_object_unref (settings);

	if (global_view_setting) {
		if (outgoing_folder) {
			view_instance = e_shell_view_new_view_instance (
				shell_view, "global_view_sent_setting");
		} else {
			view_instance = e_shell_view_new_view_instance (
				shell_view, "global_view_setting");
		}
	} else {
		view_instance = e_shell_view_new_view_instance (
			shell_view, view_id);
	}

	self->priv->view_instance = g_object_ref (view_instance);

	orientable = GTK_ORIENTABLE (view);
	orientation = gtk_orientable_get_orientation (orientable);
	show_vertical_view =
		!global_view_setting &&
		(orientation == GTK_ORIENTATION_HORIZONTAL);

	if (show_vertical_view) {
		const gchar *user_directory;
		gchar *filename;

		/* Force the view instance into vertical view. */

		g_free (view_instance->custom_filename);
		g_free (view_instance->current_view_filename);

		user_directory = gal_view_collection_get_user_directory (
			view_collection);

		filename = g_strdup_printf (
			"custom_wide_view-%s.xml", view_id);
		view_instance->custom_filename =
			g_build_filename (user_directory, filename, NULL);
		g_free (filename);

		filename = g_strdup_printf (
			"current_wide_view-%s.xml", view_id);
		view_instance->current_view_filename =
			g_build_filename (user_directory, filename, NULL);
		g_free (filename);
	}

	g_free (view_id);

	if (outgoing_folder) {
		if (show_vertical_view)
			gal_view_instance_set_default_view (
				view_instance, "Wide_View_Sent");
		else
			gal_view_instance_set_default_view (
				view_instance, "As_Sent_Folder");
	} else if (show_vertical_view) {
		gal_view_instance_set_default_view (
			view_instance, "Wide_View_Normal");
	}

	gal_view_instance_load (view_instance);

	if (!gal_view_instance_exists (view_instance)) {
		gchar *state_filename;

		state_filename = mail_config_folder_to_cachename (
			folder, "et-header-");

		if (g_file_test (state_filename, G_FILE_TEST_IS_REGULAR)) {
			GalView *gal_view;

			gal_view = gal_view_etable_new ("");

			/* XXX This only stashes the filename in the view.
			 *     The state file is not actually loaded until
			 *     the MessageList is attached to the view. */
			gal_view_load (gal_view, state_filename);

			gal_view_instance_set_custom_view (
				view_instance, gal_view);

			g_object_unref (gal_view);
		}

		g_free (state_filename);
	}

	g_signal_connect (
		view_instance, "display-view",
		G_CALLBACK (mail_paned_display_view_cb), view);

	mail_paned_display_view_cb (
		view_instance,
		gal_view_instance_get_current_view (view_instance),
		view);

	view_id = gal_view_instance_get_current_view_id (view_instance);
	e_shell_view_set_view_id (shell_view, view_id);
	g_free (view_id);

	g_object_unref (view_instance);

	g_clear_object (&folder);
}

static void
mail_paned_view_set_preview_visible (EMailView *view,
                                     gboolean preview_visible)
{
	EMailViewClass *parent_class;

	/* If we're showing the preview, tell EMailReader to reload the
	 * selected message.  This should force it to download the full
	 * message if necessary, so we don't get an empty preview. */
	if (preview_visible) {
		EMailReader *reader;
		GtkWidget *message_list;
		const gchar *cursor_uid;

		reader = E_MAIL_READER (view);
		message_list = e_mail_reader_get_message_list (reader);
		cursor_uid = MESSAGE_LIST (message_list)->cursor_uid;

		if (cursor_uid != NULL)
			e_mail_reader_set_message (reader, cursor_uid);
	}

	mail_paned_view_save_boolean (
		E_MAIL_VIEW (view),
		STATE_KEY_PREVIEW_VISIBLE, preview_visible);

	/* Chain up to parent's set_preview_visible() method. */
	parent_class = E_MAIL_VIEW_CLASS (e_mail_paned_view_parent_class);
	parent_class->set_preview_visible (view, preview_visible);
}

static guint
mail_paned_view_open_selected_mail (EMailPanedView *view)
{
	return e_mail_reader_open_selected (E_MAIL_READER (view));
}

static void
e_mail_paned_view_class_init (EMailPanedViewClass *class)
{
	GObjectClass *object_class;
	EMailViewClass *mail_view_class;

	object_class = G_OBJECT_CLASS (class);
	object_class->dispose = mail_paned_view_dispose;
	object_class->constructed = mail_paned_view_constructed;
	object_class->set_property = mail_paned_view_set_property;
	object_class->get_property = mail_paned_view_get_property;

	mail_view_class = E_MAIL_VIEW_CLASS (class);
	mail_view_class->set_search_strings = mail_paned_view_set_search_strings;
	mail_view_class->get_view_instance = mail_paned_view_get_view_instance;
	mail_view_class->update_view_instance = mail_paned_view_update_view_instance;

	mail_view_class->set_preview_visible = mail_paned_view_set_preview_visible;

	class->open_selected_mail = mail_paned_view_open_selected_mail;

	/* Inherited from EMailReader */
	g_object_class_override_property (
		object_class,
		PROP_FORWARD_STYLE,
		"forward-style");

	/* Inherited from EMailReader */
	g_object_class_override_property (
		object_class,
		PROP_GROUP_BY_THREADS,
		"group-by-threads");

	/* Inherited from EMailReader */
	g_object_class_override_property (
		object_class,
		PROP_REPLY_STYLE,
		"reply-style");

	/* Inherited from EMailReader */
	g_object_class_override_property (
		object_class,
		PROP_MARK_SEEN_ALWAYS,
		"mark-seen-always");

	/* Inherited from EMailReader */
	g_object_class_override_property (
		object_class,
		PROP_DELETE_SELECTS_PREVIOUS,
		"delete-selects-previous");

	properties[PROP_PREVIEW_TOOLBAR_VISIBLE] =
		g_param_spec_boolean (
			"preview-toolbar-visible", NULL, NULL,
			TRUE,
			G_PARAM_READWRITE |
			G_PARAM_EXPLICIT_NOTIFY |
			G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
e_mail_paned_view_reader_init (EMailReaderInterface *iface)
{
	iface->get_alert_sink = mail_paned_view_get_alert_sink;
	iface->get_backend = mail_paned_view_get_backend;
	iface->get_mail_display = mail_paned_view_get_mail_display;
	iface->get_hide_deleted = mail_paned_view_get_hide_deleted;
	iface->get_message_list = mail_paned_view_get_message_list;
	iface->get_ui_manager = mail_paned_view_get_ui_manager;
	iface->get_preview_pane = mail_paned_view_get_preview_pane;
	iface->get_window = mail_paned_view_get_window;
	iface->set_folder = mail_paned_view_set_folder;
	iface->open_selected_mail = mail_paned_view_reader_open_selected_mail;
}

static void
e_mail_paned_view_init (EMailPanedView *view)
{
	view->priv = e_mail_paned_view_get_instance_private (view);

	e_signal_connect_notify (
		view, "notify::group-by-threads",
		G_CALLBACK (mail_paned_view_notify_group_by_threads_cb),
		NULL);
}

GtkWidget *
e_mail_paned_view_new (EShellView *shell_view)
{
	g_return_val_if_fail (E_IS_SHELL_VIEW (shell_view), NULL);

	return g_object_new (
		E_TYPE_MAIL_PANED_VIEW,
		"shell-view", shell_view, NULL);
}

void
e_mail_paned_view_hide_message_list_pane (EMailPanedView *view,
                                          gboolean visible)
{
	g_return_if_fail (E_IS_MAIL_PANED_VIEW (view));

	if (visible)
		gtk_widget_show (view->priv->list_column);
	else
		gtk_widget_hide (view->priv->list_column);
}

gboolean
e_mail_paned_view_get_select_mode (EMailPanedView *view)
{
	g_return_val_if_fail (E_IS_MAIL_PANED_VIEW (view), FALSE);

	return view->priv->select_mode;
}

void
e_mail_paned_view_set_select_mode (EMailPanedView *view,
				   gboolean select_mode)
{
	ESelectionModel *selection;

	g_return_if_fail (E_IS_MAIL_PANED_VIEW (view));

	view->priv->select_mode = select_mode;

	if (view->priv->message_list) {
		selection = e_tree_get_selection_model (E_TREE (view->priv->message_list));
		/* Read by ETableItem: plain clicks toggle rows like Ctrl+click */
		g_object_set_data (G_OBJECT (selection), "oma-toggle-select", GINT_TO_POINTER (select_mode ? 1 : 0));

		if (select_mode)
			gtk_widget_grab_focus (view->priv->message_list);
	}

	mail_paned_view_update_selection_bar (view);
}

GtkWidget *
e_mail_paned_view_get_preview (EMailPanedView *view)
{
	g_return_val_if_fail (E_IS_MAIL_PANED_VIEW (view), NULL);

	return GTK_WIDGET (mail_paned_view_get_mail_display (E_MAIL_READER (view)));
}

gboolean
e_mail_paned_view_get_preview_toolbar_visible (EMailPanedView *view)
{
	g_return_val_if_fail (E_IS_MAIL_PANED_VIEW (view), FALSE);

	return view->priv->preview_toolbar_visible;
}

void
e_mail_paned_view_set_preview_toolbar_visible (EMailPanedView *view,
					       gboolean value)
{
	g_return_if_fail (E_IS_MAIL_PANED_VIEW (view));

	if (!view->priv->preview_toolbar_visible == !value)
		return;

	view->priv->preview_toolbar_visible = value;

	gtk_widget_set_visible (view->priv->preview_toolbar_box, value);

	g_object_notify_by_pspec (G_OBJECT (view), properties[PROP_PREVIEW_TOOLBAR_VISIBLE]);
}

static void
mail_paned_view_style_preview_toolbar (GtkToolbar *toolbar)
{
	gint ii, n_items = gtk_toolbar_get_n_items (toolbar);
	gboolean seen_primary = FALSE, seen_spacer = FALSE;

	for (ii = 0; ii < n_items; ii++) {
		GtkToolItem *item = gtk_toolbar_get_nth_item (toolbar, ii);
		GtkStyleContext *context = gtk_widget_get_style_context (GTK_WIDGET (item));

		if (GTK_IS_SEPARATOR_TOOL_ITEM (item)) {
			/* The first separator becomes a spring between the groups. */
			gboolean spacer = !seen_spacer;

			gtk_separator_tool_item_set_draw (GTK_SEPARATOR_TOOL_ITEM (item), FALSE);
			gtk_tool_item_set_expand (item, spacer);
			seen_spacer = TRUE;
			continue;
		}

		if (!seen_spacer && gtk_tool_item_get_is_important (item)) {
			gtk_style_context_add_class (context, "oma-pill");
			if (!seen_primary)
				gtk_style_context_add_class (context, "oma-pill-primary");
			seen_primary = TRUE;
		}
	}
}

static void
mail_paned_view_preview_toolbar_allocate_cb (GtkWidget *toolbar,
					     GdkRectangle *allocation,
					     gpointer user_data)
{
	gint n_items = gtk_toolbar_get_n_items (GTK_TOOLBAR (toolbar));

	/* The UI manager fills (and, when customized, refills) the toolbar
	 * with gtk_toolbar_insert(), which emits no "add"; restyle whenever
	 * the set of items changes. */
	if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (toolbar), "oma-styled-items")) == n_items)
		return;

	g_object_set_data (G_OBJECT (toolbar), "oma-styled-items", GINT_TO_POINTER (n_items));
	mail_paned_view_style_preview_toolbar (GTK_TOOLBAR (toolbar));
}

void
e_mail_paned_view_take_preview_toolbar (EMailPanedView *self,
					GtkWidget *toolbar)
{
	g_return_if_fail (E_IS_MAIL_PANED_VIEW (self));
	g_return_if_fail (GTK_IS_WIDGET (toolbar));

	gtk_widget_set_visible (toolbar, TRUE);

	/* Reader action bar: labelled reply pills on the left, with Reply as
	 * the one filled button; the quieter icon actions sit on the right. */
	/* The toolbar's widget name is its UI-manager id, so style it
	 * through the surrounding box instead. */
	gtk_widget_set_name (self->priv->preview_toolbar_box, "omamail-preview-bar");
	if (GTK_IS_TOOLBAR (toolbar)) {
		gtk_toolbar_set_style (GTK_TOOLBAR (toolbar), GTK_TOOLBAR_BOTH_HORIZ);
		mail_paned_view_style_preview_toolbar (GTK_TOOLBAR (toolbar));
		g_signal_connect (toolbar, "size-allocate",
			G_CALLBACK (mail_paned_view_preview_toolbar_allocate_cb), NULL);
	}

	gtk_box_pack_start (GTK_BOX (self->priv->preview_toolbar_box), toolbar, FALSE, FALSE, 0);
}

/**
 * e_mail_paned_view_take_list_filters:
 * @self: an #EMailPanedView
 * @filters: (transfer full): a widget holding the list filter chips
 *
 * Places @filters in the header above the message list.
 **/
void
e_mail_paned_view_take_list_filters (EMailPanedView *self,
				     GtkWidget *filters)
{
	g_return_if_fail (E_IS_MAIL_PANED_VIEW (self));
	g_return_if_fail (GTK_IS_WIDGET (filters));

	gtk_box_pack_start (GTK_BOX (self->priv->list_filters_box), filters, TRUE, TRUE, 0);
	gtk_widget_show (filters);
	gtk_widget_set_no_show_all (self->priv->list_filters_box, FALSE);
	gtk_widget_show (self->priv->list_filters_box);
}
