/*
 * SPDX-FileCopyrightText: (C) 1999-2008 Novell, Inc. (www.novell.com)
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileContributor: Miguel Angel Lopez Hernandez <miguel@gulev.org.mx>
 */

#include "evolution-config.h"

#include <string.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gio/gio.h>

#ifndef G_OS_WIN32
#include <gio/gdesktopappinfo.h>
#endif

#ifdef HAVE_CANBERRA
#include <canberra-gtk.h>
#endif

#include <time.h>

#include "libemail-engine/libemail-engine.h"
#include "mail/e-mail-account-store.h"
#include "mail/e-mail-backend.h"
#include "mail/e-mail-ui-session.h"
#include "mail/e-mail-view.h"
#include "mail/em-utils.h"
#include "mail/em-event.h"
#include "mail/em-folder-tree.h"
#include "mail/message-list.h"
#include "shell/e-shell-view.h"

#ifdef HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif

#ifdef HAVE_LIBUNITY
#include <unity.h>
#endif

#define CONF_KEY_NOTIFY_ONLY_INBOX	"notify-only-inbox"
#define CONF_KEY_ENABLED_STATUS	        "notify-status-enabled"
#define CONF_KEY_STATUS_NOTIFICATION	"notify-status-notification"
#define CONF_KEY_ENABLED_SOUND		"notify-sound-enabled"

#define GNOME_NOTIFICATIONS_PANEL_DESKTOP "gnome-notifications-panel.desktop"

static gboolean enabled = FALSE;
static GMutex mlock;
static gulong not_accounts_handler_id = 0;
static GHashTable *not_accounts = NULL; /* gchar * ~> NULL; UIDs of accounts which have disabled notifications */

/**
 * each part should "implement" its own "public" functions:
 * a) void new_notify_... (EMEventTargetFolder *t)
 *    when new_notify message is sent by Evolution
 *
 * b) void read_notify_... (EMEventTargetMessage *t)
 *    it is called when read_notify message is sent by Evolution
 *
 * c) void enable_... (gint enable)
 *    when plugin itself or the part is enabled/disabled
 *
 * d) GtkWidget *get_config_widget_...(void)
 *    to obtain config widget for the particular part
 *
 * It also should have its own GSettings key for enabled state. In each particular
 * function it should do its work as expected. enable_... will be called always
 * when disabling plugin, but only when enabling/disabling part itself.
 **/

/* -------------------------------------------------------------------  */
/*                       Helper functions                               */
/* -------------------------------------------------------------------  */

static gboolean
is_part_enabled (const gchar *key)
{
	/* the part is enabled by default */
	gboolean res = TRUE;
	GSettings *settings;

	settings = e_util_ref_settings ("org.gnome.evolution.plugin.mail-notification");

	res = g_settings_get_boolean (settings, key);

	g_object_unref (settings);

	return res;
}

static gboolean
can_notify_account (CamelStore *store)
{
	gboolean can_notify;
	const gchar *uid;

	if (!store)
		return TRUE;

	g_mutex_lock (&mlock);

	uid = camel_service_get_uid (CAMEL_SERVICE (store));
	can_notify = !uid || !not_accounts || !g_hash_table_contains (not_accounts, uid);

	g_mutex_unlock (&mlock);

	return can_notify;
}

static void
mail_notify_not_accounts_changed_locked (GSettings *settings)
{
	gchar **uids;

	g_return_if_fail (G_IS_SETTINGS (settings));

	uids = g_settings_get_strv (settings, "notify-not-accounts");

	if (uids && *uids) {
		gint ii;

		if (!not_accounts)
			not_accounts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

		g_hash_table_remove_all (not_accounts);

		for (ii = 0; uids[ii]; ii++) {
			g_hash_table_insert (not_accounts, g_strdup (uids[ii]), NULL);
		}
	} else {
		g_clear_pointer (&not_accounts, g_hash_table_destroy);
	}

	g_strfreev (uids);
}

static void
mail_notify_not_accounts_changed_cb (GSettings *settings,
				     const gchar *key,
				     gpointer user_data)
{
	g_return_if_fail (G_IS_SETTINGS (settings));

	g_mutex_lock (&mlock);
	mail_notify_not_accounts_changed_locked (settings);
	g_mutex_unlock (&mlock);
}

/* -------------------------------------------------------------------  */
/*                           DBUS part                                  */
/* -------------------------------------------------------------------  */

#define DBUS_PATH		"/org/gnome/evolution/mail/newmail"
#define DBUS_INTERFACE		"org.gnome.evolution.mail.dbus.Signal"

static GDBusConnection *connection = NULL;

static gboolean init_gdbus (void);

static void
send_dbus_message (const gchar *name,
                   const gchar *display_name,
                   guint new_count,
                   const gchar *msg_uid,
                   const gchar *msg_sender,
                   const gchar *msg_subject)
{
	GDBusMessage *message;
	GVariantBuilder *builder;
	GError *error = NULL;

	g_return_if_fail (name != NULL);
	g_return_if_fail (display_name != NULL);
	g_return_if_fail (g_utf8_validate (name, -1, NULL));
	g_return_if_fail (g_utf8_validate (display_name, -1, NULL));
	g_return_if_fail (msg_uid == NULL || g_utf8_validate (msg_uid, -1, NULL));
	g_return_if_fail (msg_sender == NULL || g_utf8_validate (msg_sender, -1, NULL));
	g_return_if_fail (msg_subject == NULL || g_utf8_validate (msg_subject, -1, NULL));

	/* Create a new message on the DBUS_INTERFACE */
	if (!(message = g_dbus_message_new_signal (DBUS_PATH, DBUS_INTERFACE, name)))
		return;

	builder = g_variant_builder_new (G_VARIANT_TYPE_TUPLE);

	/* Appends the data as an argument to the message */
	g_variant_builder_add (builder, "(s)", display_name);

	if (new_count) {
		g_variant_builder_add (builder, "(s)", display_name);
		g_variant_builder_add (builder, "(u)", new_count);
	}

	#define add_named_param(name, value) \
		if (value) { \
			gchar *val; \
			val = g_strconcat (name, ":", value, NULL); \
			g_variant_builder_add (builder, "(s)", val); \
			g_free (val); \
		}

	add_named_param ("msg_uid", msg_uid);
	add_named_param ("msg_sender", msg_sender);
	add_named_param ("msg_subject", msg_subject);

	#undef add_named_param

	g_dbus_message_set_body (message, g_variant_builder_end (builder));
	g_variant_builder_unref (builder);

	/* Sends the message */
	g_dbus_connection_send_message (
		connection, message,
		G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, &error);

	/* Frees the message */
	g_object_unref (message);

	if (error != NULL) {
		g_warning ("%s: %s", G_STRFUNC, error->message);
		g_error_free (error);
	}
}

static gboolean
reinit_gdbus (gpointer user_data)
{
	if (!enabled || init_gdbus ())
		return FALSE;

	/* keep trying to re-establish dbus connection */
	return TRUE;
}

static void
connection_closed_cb (GDBusConnection *pconnection,
                      gboolean remote_peer_vanished,
                      GError *error,
                      gpointer user_data)
{
	g_return_if_fail (connection != pconnection);

	g_object_unref (connection);
	connection = NULL;

	e_named_timeout_add (3000, reinit_gdbus, NULL);
}

static gboolean
init_gdbus (void)
{
	GError *error = NULL;

	if (connection != NULL)
		return TRUE;

	connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
	if (error != NULL) {
		g_warning ("could not get system bus: %s\n", error->message);
		g_error_free (error);

		return FALSE;
	}

	g_dbus_connection_set_exit_on_close (connection, FALSE);

	g_signal_connect (
		connection, "closed",
		G_CALLBACK (connection_closed_cb), NULL);

	return TRUE;
}

/* -------------------------------------------------------------------  */

static void
new_notify_dbus (EMEventTargetFolder *t)
{
	if (connection != NULL)
		send_dbus_message (
			"Newmail", t->display_name, t->new,
			t->msg_uid, t->msg_sender, t->msg_subject);
}

static void
read_notify_dbus (EMEventTargetMessage *t)
{
	if (connection != NULL)
		send_dbus_message (
			"MessageReading",
			camel_folder_get_display_name (t->folder),
			0, NULL, NULL, NULL);
}

static void
enable_dbus (gint enable)
{
	if (enable) {
		/* we ignore errors here */
		init_gdbus ();
	} else {
		g_clear_object (&connection);
	}
}

/* -------------------------------------------------------------------  */
/*                     Notification area part                           */
/* -------------------------------------------------------------------  */

#ifdef HAVE_LIBNOTIFY

/* Omarchy Mail: every new message gets its own notification, titled with
 * the sender and showing the subject plus a short body preview. Large
 * bursts are capped by the folder cache, which reports the older part of
 * a burst as a single count ("N more new messages"). */

#define OMA_PREVIEW_MAX_CHARS 160
#define OMA_PREVIEW_WAIT_MS 2500

typedef struct _MailNotifyItem {
	gchar *key;		/* folder_uri "\n" msg_uid; msg_uid empty for a summary */
	gchar *folder_uri;
	gchar *msg_uid;		/* NULL for the "N more" summary */
	gchar *title;
	gchar *subject;
	gchar *preview;
	gchar *display_name;	/* folder display name, like "Inbox" */
	guint extra_count;	/* summary only */
	gboolean shown;
	guint wait_id;		/* waiting for a preview before showing */
	NotifyNotification *notification;
} MailNotifyItem;

static GHashTable *notify_items = NULL; /* gchar *key ~> MailNotifyItem * */
static GHashTable *unread_messages_by_folder = NULL;

#ifdef HAVE_LIBUNITY
static guint unread_message_count = 0;

static void
update_unity_launcher_entry (void)
{
	UnityLauncherEntry *entry = unity_launcher_entry_get_for_desktop_id ("org.gnome.Evolution.desktop");

	if (entry) {
		unity_launcher_entry_set_count (entry, unread_message_count);
		unity_launcher_entry_set_count_visible (entry, unread_message_count != 0);
	}
}
#endif

static gchar *
mail_notify_make_key (const gchar *folder_uri,
		      const gchar *msg_uid)
{
	return g_strconcat (folder_uri ? folder_uri : "", "\n", msg_uid ? msg_uid : "", NULL);
}

static void
mail_notify_item_free (gpointer ptr)
{
	MailNotifyItem *item = ptr;

	if (!item)
		return;

	if (item->wait_id)
		g_source_remove (item->wait_id);

	if (item->notification) {
		g_signal_handlers_disconnect_matched (item->notification, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, item);
		g_object_unref (item->notification);
	}

	g_free (item->key);
	g_free (item->folder_uri);
	g_free (item->msg_uid);
	g_free (item->title);
	g_free (item->subject);
	g_free (item->preview);
	g_free (item->display_name);
	g_slice_free (MailNotifyItem, item);
}

static void
mail_notify_close_item (MailNotifyItem *item)
{
	g_return_if_fail (item != NULL);

	if (item->notification && item->shown)
		notify_notification_close (item->notification, NULL);

	/* Frees the item */
	g_hash_table_remove (notify_items, item->key);
}

static void
mail_notify_close_by_uid (const gchar *folder_uri,
			  const gchar *msg_uid)
{
	MailNotifyItem *item;
	gchar *key;

	if (!notify_items)
		return;

	key = mail_notify_make_key (folder_uri, msg_uid);
	item = g_hash_table_lookup (notify_items, key);
	if (item)
		mail_notify_close_item (item);
	g_free (key);
}

static gboolean
mail_notify_close_key_idle_cb (gpointer user_data)
{
	const gchar *key = user_data;
	MailNotifyItem *item;

	item = notify_items ? g_hash_table_lookup (notify_items, key) : NULL;
	if (item)
		mail_notify_close_item (item);

	return G_SOURCE_REMOVE;
}

/* For use from inside the notification's own action callbacks, where
 * the notification object must stay alive until the callback returns. */
static void
mail_notify_close_by_uid_later (const gchar *folder_uri,
				const gchar *msg_uid)
{
	g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, mail_notify_close_key_idle_cb,
		mail_notify_make_key (folder_uri, msg_uid), g_free);
}

static void
remove_notification (void)
{
	if (notify_items) {
		GList *items, *link;

		items = g_hash_table_get_values (notify_items);
		for (link = items; link; link = g_list_next (link)) {
			MailNotifyItem *item = link->data;

			if (item->notification && item->shown)
				notify_notification_close (item->notification, NULL);
		}
		g_list_free (items);

		g_hash_table_remove_all (notify_items);
	}

#ifdef HAVE_LIBUNITY
	unread_message_count = 0;
	update_unity_launcher_entry ();
#endif
}

static EMailSession *
mail_notify_get_session (void)
{
	EShell *shell;
	EShellBackend *backend;

	shell = e_shell_get_default ();
	if (!shell)
		return NULL;

	backend = e_shell_get_backend_by_name (shell, "mail");
	if (!backend)
		return NULL;

	return e_mail_backend_get_session (E_MAIL_BACKEND (backend));
}

/* Returns a new reference on the already opened folder, if any; never blocks. */
static CamelFolder *
mail_notify_ref_open_folder (const gchar *folder_uri)
{
	EMailSession *session;
	CamelStore *store = NULL;
	CamelFolder *folder = NULL;
	gchar *folder_name = NULL;

	session = mail_notify_get_session ();
	if (!session || !folder_uri)
		return NULL;

	if (e_mail_folder_uri_parse (CAMEL_SESSION (session), folder_uri, &store, &folder_name, NULL)) {
		folder = mail_folder_cache_ref_folder (
			e_mail_session_get_folder_cache (session), store, folder_name);
	}

	g_clear_object (&store);
	g_free (folder_name);

	return folder;
}

static gchar *
mail_notify_sender_title (const gchar *msg_sender)
{
	CamelInternetAddress *address;
	const gchar *name = NULL, *email = NULL;
	gchar *title = NULL;

	if (!msg_sender || !*msg_sender)
		return g_strdup (_("New message"));

	address = camel_internet_address_new ();
	if (camel_address_unformat (CAMEL_ADDRESS (address), msg_sender) > 0 &&
	    camel_internet_address_get (address, 0, &name, &email)) {
		if (name && *name)
			title = g_strdup (name);
		else if (email && *email)
			title = g_strdup (email);
	}
	g_object_unref (address);

	return title ? title : g_strdup (msg_sender);
}

/* Collapses whitespace and limits the length, so the preview reads as one
 * or two tidy lines in the notification. */
static gchar *
mail_notify_tidy_preview (const gchar *text)
{
	GString *tidy;
	const gchar *ptr;
	gboolean last_space = TRUE;

	if (!text || !*text || !g_utf8_validate (text, -1, NULL))
		return NULL;

	tidy = g_string_sized_new (OMA_PREVIEW_MAX_CHARS + 8);

	for (ptr = text; *ptr && g_utf8_strlen (tidy->str, -1) < OMA_PREVIEW_MAX_CHARS; ptr = g_utf8_next_char (ptr)) {
		gunichar chr = g_utf8_get_char (ptr);

		if (g_unichar_isspace (chr) || chr == 0x00A0 || chr == 0x200B || chr == 0x034F) {
			if (!last_space)
				g_string_append_c (tidy, ' ');
			last_space = TRUE;
		} else if (!g_unichar_iscntrl (chr)) {
			g_string_append_unichar (tidy, chr);
			last_space = FALSE;
		}
	}

	while (tidy->len > 0 && tidy->str[tidy->len - 1] == ' ')
		g_string_truncate (tidy, tidy->len - 1);

	if (*ptr && tidy->len > 0)
		g_string_append (tidy, "…");

	if (!tidy->len) {
		g_string_free (tidy, TRUE);
		return NULL;
	}

	return g_string_free (tidy, FALSE);
}

/* -------------------------------------------------------------------  */

typedef struct _NotifyDefaultActionData NotifyDefaultActionData;

struct _NotifyDefaultActionData {
	gchar *folder_uri;
	gchar *msg_uid;
	guint32 set_flags;
};

static NotifyDefaultActionData *
notify_default_action_data_new (const gchar *folder_uri,
                                const gchar *msg_uid,
				guint32 set_flags)
{
	NotifyDefaultActionData *result;
	result = g_slice_new (NotifyDefaultActionData);
	result->folder_uri = g_strdup (folder_uri);
	result->msg_uid = g_strdup (msg_uid);
	result->set_flags = set_flags;
	return result;
}

static void
notify_default_action_free_cb (gpointer user_data)
{
	NotifyDefaultActionData *data = user_data;
	g_free (data->folder_uri);
	g_free (data->msg_uid);
	g_slice_free (NotifyDefaultActionData, data);
}

static void
notify_default_action_cb (NotifyNotification *notification,
                          gchar *label,
                          gpointer user_data)
{
	NotifyDefaultActionData *data = user_data;
	EShell *shell;
	EShellView *shell_view;
	EShellWindow *shell_window;
	EShellSidebar *shell_sidebar;
	EMFolderTree *folder_tree;
	EUIAction *action;
	GtkApplication *application;
	GList *list, *fallback = NULL;
	gchar *folder_uri, *msg_uid;

	/* The data is freed together with the notification below. */
	folder_uri = g_strdup (data->folder_uri);
	msg_uid = g_strdup (data->msg_uid);

	shell = e_shell_get_default ();
	application = GTK_APPLICATION (shell);
	list = gtk_application_get_windows (application);

	/* Find the first EShellWindow in the list. */
	while (list != NULL) {
		if (E_IS_SHELL_WINDOW (list->data)) {
			if (!fallback)
				fallback = list;

			if (g_strcmp0 (e_shell_window_get_active_view (list->data), "mail") == 0)
				break;
		}

		list = g_list_next (list);
	}

	if (!list)
		list = fallback;

	if (!list) {
		/* Running in the background without any window yet. */
		e_shell_create_shell_window (shell, "mail");
		list = gtk_application_get_windows (application);
		while (list && !E_IS_SHELL_WINDOW (list->data))
			list = g_list_next (list);
	}

	if (!list) {
		g_warn_if_reached ();
		g_free (folder_uri);
		g_free (msg_uid);
		return;
	}

	/* Present the shell window (this also shows a window hidden
	 * by running in the background). */
	shell_window = E_SHELL_WINDOW (list->data);
	gtk_window_present (GTK_WINDOW (shell_window));

	/* Switch to the mail view. */
	shell_view = e_shell_window_get_shell_view (shell_window, "mail");
	action = e_shell_view_get_switcher_action (shell_view);
	e_ui_action_set_active (action, TRUE);

	/* Select the folder with the new mail. */
	shell_sidebar = e_shell_view_get_shell_sidebar (shell_view);
	g_object_get (shell_sidebar, "folder-tree", &folder_tree, NULL);
	em_folder_tree_set_selected (folder_tree, folder_uri, FALSE);
	g_clear_object (&folder_tree);

	if (msg_uid) {
		EMailView *mail_view = NULL;

		g_object_get (e_shell_view_get_shell_content (shell_view), "mail-view", &mail_view, NULL);
		if (mail_view) {
			MessageList *message_list;

			/* Select the message. */
			message_list = MESSAGE_LIST (e_mail_reader_get_message_list (E_MAIL_READER (mail_view)));
			message_list_select_uid (message_list, msg_uid, TRUE);

			g_clear_object (&mail_view);
		}
	}

	mail_notify_close_by_uid_later (folder_uri, msg_uid);

	g_free (folder_uri);
	g_free (msg_uid);
}

static void
uri_to_folder_cb (GObject *source_object,
		  GAsyncResult *result,
		  gpointer user_data)
{
	NotifyDefaultActionData *data = user_data;
	EMailSession *session = E_MAIL_SESSION (source_object);
	CamelFolder *folder;
	GError *error = NULL;

	folder = e_mail_session_uri_to_folder_finish (session, result, &error);

	if (!folder) {
		if (error) {
			g_warning ("Failed to get folder: %s", error->message);
			g_clear_error (&error);
		}
		notify_default_action_free_cb (data);
	        return;
	}

	camel_folder_set_message_flags (folder, data->msg_uid, data->set_flags, data->set_flags);

	g_object_unref (folder);

	notify_default_action_free_cb (data);
}

static void
notify_set_flags_action_cb (NotifyNotification *notification,
			    gchar *action,
			    gpointer user_data)
{
	NotifyDefaultActionData *data = user_data;
	NotifyDefaultActionData *async_data;
	EMailSession *session;

	if (!data || !data->folder_uri || !data->msg_uid)
		return;

	session = mail_notify_get_session ();
	if (!session)
		return;

	async_data = notify_default_action_data_new (data->folder_uri, data->msg_uid, data->set_flags);
	e_mail_session_uri_to_folder (
		session,
		async_data->folder_uri,
		0,
		G_PRIORITY_DEFAULT,
		NULL,
		uri_to_folder_cb,
		async_data);

	mail_notify_close_by_uid_later (async_data->folder_uri, async_data->msg_uid);
}

/* Check if actions are supported by the notification daemon.
 * This is really only for Ubuntu's Notify OSD, which does not
 * support actions.  Pretty much all other implementations do. */
static gboolean
can_support_actions (void)
{
	static gboolean supports_actions = FALSE;
	static gboolean have_checked = FALSE;

	if (!have_checked) {
		GList *caps = NULL;
		GList *match;

		have_checked = TRUE;

		caps = notify_get_server_caps ();

		match = g_list_find_custom (
			caps, "actions", (GCompareFunc) strcmp);
		supports_actions = (match != NULL);

		g_list_foreach (caps, (GFunc) g_free, NULL);
		g_list_free (caps);
	}

	return supports_actions;
}

static void
mail_notify_notification_closed_cb (NotifyNotification *notification,
				    gpointer user_data)
{
	MailNotifyItem *item = user_data;

	/* Dismissed by the user or expired; forget it. */
	if (notify_items && g_hash_table_lookup (notify_items, item->key) == item)
		g_hash_table_remove (notify_items, item->key);
}

static void
mail_notify_item_update (MailNotifyItem *item)
{
	GString *body;
	const gchar *icon_name = "org.omarchy.Mail";

	body = g_string_sized_new (256);

	if (item->msg_uid) {
		if (item->subject && *item->subject) {
			gchar *escaped = g_markup_escape_text (item->subject, -1);
			g_string_append_printf (body, "<b>%s</b>", escaped);
			g_free (escaped);
		} else {
			g_string_append (body, _("(No Subject)"));
		}

		if (item->preview && *item->preview) {
			gchar *escaped = g_markup_escape_text (item->preview, -1);
			g_string_append_c (body, '\n');
			g_string_append (body, escaped);
			g_free (escaped);
		}
	} else if (item->display_name) {
		gchar *escaped = g_markup_escape_text (item->display_name, -1);
		g_string_append_printf (body, _("In %s"), escaped);
		g_free (escaped);
	}

	if (!notify_is_initted () && !notify_init ("evolution-mail-notification"))
		g_warning ("%s: notify init error", G_STRFUNC);

	if (item->notification) {
		notify_notification_update (item->notification, item->title, body->str, icon_name);
	} else {
		item->notification = notify_notification_new (item->title, body->str, icon_name);

		notify_notification_set_urgency (item->notification, NOTIFY_URGENCY_NORMAL);
		notify_notification_set_timeout (item->notification, NOTIFY_EXPIRES_DEFAULT);
		notify_notification_set_app_name (item->notification, _("Mail"));
		notify_notification_set_hint (
			item->notification, "desktop-entry",
			g_variant_new_string ("org.omarchy.Mail"));
		notify_notification_set_category (item->notification, "email.arrived");

		if (e_util_is_running_gnome ()) {
			notify_notification_set_hint (
				item->notification, "sound-name",
				g_variant_new_string ("message-new-email"));
		}

		g_signal_connect (item->notification, "closed",
			G_CALLBACK (mail_notify_notification_closed_cb), item);

		if (can_support_actions ()) {
			notify_notification_add_action (
				item->notification, "default", _("Open"),
				notify_default_action_cb,
				notify_default_action_data_new (item->folder_uri, item->msg_uid, 0),
				notify_default_action_free_cb);

			if (item->msg_uid) {
				notify_notification_add_action (
					item->notification, "mark-read", _("Mark as Read"),
					notify_set_flags_action_cb,
					notify_default_action_data_new (item->folder_uri, item->msg_uid,
						CAMEL_MESSAGE_SEEN),
					notify_default_action_free_cb);

				notify_notification_add_action (
					item->notification, "delete", _("Delete"),
					notify_set_flags_action_cb,
					notify_default_action_data_new (item->folder_uri, item->msg_uid,
						CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_DELETED),
					notify_default_action_free_cb);
			}
		}
	}

	g_string_free (body, TRUE);
}

static void
mail_notify_item_show (MailNotifyItem *item)
{
	GError *error = NULL;

	if (item->wait_id) {
		g_source_remove (item->wait_id);
		item->wait_id = 0;
	}

	mail_notify_item_update (item);

	if (!notify_notification_show (item->notification, &error)) {
		g_warning ("%s: %s", G_STRFUNC, error ? error->message : "Unknown error");
		g_clear_error (&error);
	} else {
		item->shown = TRUE;
	}
}

static gboolean
mail_notify_preview_wait_cb (gpointer user_data)
{
	MailNotifyItem *item = user_data;

	/* The body is taking long to download; show what is known now,
	 * the preview is filled in when it arrives. */
	item->wait_id = 0;
	mail_notify_item_show (item);

	return G_SOURCE_REMOVE;
}

static void
mail_notify_got_message_cb (GObject *source_object,
			    GAsyncResult *result,
			    gpointer user_data)
{
	gchar *key = user_data;
	CamelMimeMessage *message;
	MailNotifyItem *item;

	message = camel_folder_get_message_finish (CAMEL_FOLDER (source_object), result, NULL);
	item = notify_items ? g_hash_table_lookup (notify_items, key) : NULL;

	if (item) {
		if (message) {
			gchar *preview;

			preview = camel_mime_part_generate_preview (CAMEL_MIME_PART (message), NULL, NULL);
			g_free (item->preview);
			item->preview = mail_notify_tidy_preview (preview);
			g_free (preview);
		}

		if (!item->shown) {
			mail_notify_item_show (item);
		} else if (item->preview) {
			/* Shown already (the download was slow); refresh in place. */
			mail_notify_item_show (item);
		}
	}

	g_clear_object (&message);
	g_free (key);
}

static void
mail_notify_new_message (EMEventTargetFolder *t)
{
	MailNotifyItem *item;
	CamelFolder *folder;
	gchar *key;

	key = mail_notify_make_key (t->folder_name, t->msg_uid);

	/* Already announced */
	if (g_hash_table_contains (notify_items, key)) {
		g_free (key);
		return;
	}

	item = g_slice_new0 (MailNotifyItem);
	item->key = key;
	item->folder_uri = g_strdup (t->folder_name);
	item->msg_uid = g_strdup (t->msg_uid);
	item->title = mail_notify_sender_title (t->msg_sender);
	item->subject = g_strdup (t->msg_subject);
	item->display_name = g_strdup (t->display_name);

	g_hash_table_insert (notify_items, item->key, item);

	folder = mail_notify_ref_open_folder (t->folder_name);
	if (folder) {
		CamelMessageInfo *info;

		info = camel_folder_get_message_info (folder, t->msg_uid);
		if (info) {
			item->preview = mail_notify_tidy_preview (camel_message_info_get_preview (info));
			g_object_unref (info);
		}

		if (!item->preview) {
			/* Download the message to build a preview, but do not keep
			 * the notification waiting for it for long. */
			item->wait_id = e_named_timeout_add (
				OMA_PREVIEW_WAIT_MS, mail_notify_preview_wait_cb, item);

			camel_folder_get_message (
				folder, t->msg_uid, G_PRIORITY_DEFAULT, NULL,
				mail_notify_got_message_cb, g_strdup (item->key));
		}

		g_object_unref (folder);
	}

	if (!item->wait_id)
		mail_notify_item_show (item);
}

static void
mail_notify_new_summary (EMEventTargetFolder *t)
{
	MailNotifyItem *item;
	gchar *key;

	key = mail_notify_make_key (t->folder_name, NULL);
	item = g_hash_table_lookup (notify_items, key);

	if (item) {
		g_free (key);
	} else {
		item = g_slice_new0 (MailNotifyItem);
		item->key = key;
		item->folder_uri = g_strdup (t->folder_name);
		item->display_name = g_strdup (t->display_name);

		g_hash_table_insert (notify_items, item->key, item);
	}

	item->extra_count += t->new;

	g_free (item->title);
	item->title = g_strdup_printf (ngettext (
		/* Translators: '%d' is the count of further new mails, besides
		 * those which got their own notification. */
		"%d more new message",
		"%d more new messages",
		item->extra_count), item->extra_count);

	mail_notify_item_show (item);
}

static void
new_notify_status (EMEventTargetFolder *t)
{
	if (!notify_items)
		notify_items = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, mail_notify_item_free);

	if (t->new == 1 && t->msg_uid)
		mail_notify_new_message (t);
	else
		mail_notify_new_summary (t);
}

static void
unread_notify_status (EMEventTargetFolderUnread *t)
{
	gpointer lookup;
	guint old_unread;

	if (unread_messages_by_folder == NULL)
		unread_messages_by_folder = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	lookup = g_hash_table_lookup (unread_messages_by_folder, t->folder_uri);
	old_unread = lookup == NULL ? 0 : GPOINTER_TO_UINT (lookup);

	/* Some message in the folder was read or removed: dismiss the
	 * notifications of those messages which are not new anymore. */
	if (t->unread < old_unread && notify_items) {
		CamelFolder *folder;
		GList *items, *link;

		folder = mail_notify_ref_open_folder (t->folder_uri);
		items = g_hash_table_get_values (notify_items);

		for (link = items; link; link = g_list_next (link)) {
			MailNotifyItem *item = link->data;
			CamelMessageInfo *info;
			gboolean still_new = FALSE;

			if (g_strcmp0 (item->folder_uri, t->folder_uri) != 0)
				continue;

			if (!item->msg_uid) {
				if (t->unread == 0)
					mail_notify_close_item (item);
				continue;
			}

			info = folder ? camel_folder_get_message_info (folder, item->msg_uid) : NULL;
			if (info) {
				guint32 flags = camel_message_info_get_flags (info);

				still_new = (flags & (CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_DELETED)) == 0;
				g_object_unref (info);
			}

			if (!still_new && (folder || t->unread == 0))
				mail_notify_close_item (item);
		}

		g_list_free (items);
		g_clear_object (&folder);
	}
#ifdef HAVE_LIBUNITY
	else if (t->is_inbox) {
		unread_message_count += t->unread - old_unread;
		update_unity_launcher_entry ();
	}
#endif

	if (t->unread != old_unread) {
		if (t->unread) {
			g_hash_table_insert (unread_messages_by_folder, g_strdup (t->folder_uri), GUINT_TO_POINTER (t->unread));
		} else {
			g_hash_table_remove (unread_messages_by_folder, t->folder_uri);
		}
	}
}

static void
read_notify_status (EMEventTargetMessage *t)
{
	gchar *folder_uri;

	if (!t->folder || !t->uid)
		return;

	/* The message is being shown; its notification is not needed anymore. */
	folder_uri = e_mail_folder_uri_from_folder (t->folder);
	mail_notify_close_by_uid (folder_uri, t->uid);
	g_free (folder_uri);
}
#endif

/* -------------------------------------------------------------------  */
/*                         Sound part                                   */
/* -------------------------------------------------------------------  */

/* min no. seconds between newmail notifications */
#define NOTIFY_THROTTLE 30

#define CONF_KEY_SOUND_BEEP		"notify-sound-beep"
#define CONF_KEY_SOUND_FILE		"notify-sound-file"
#define CONF_KEY_SOUND_PLAY_FILE	"notify-sound-play-file"
#define CONF_KEY_SOUND_USE_THEME        "notify-sound-use-theme"

#ifdef HAVE_CANBERRA
static ca_context *mailnotification = NULL;
#endif

static gint eca_debug = -1;

static void
do_play_sound (gboolean beep,
               gboolean use_theme,
               const gchar *file)
{
	if (eca_debug == -1)
		eca_debug = g_strcmp0 (g_getenv ("ECA_DEBUG"), "1") == 0 ? 1 : 0;

	if (!beep) {
#ifdef HAVE_CANBERRA
		gint err;

		if (!use_theme && file && *file)
			err = ca_context_play (
				mailnotification, 0,
				CA_PROP_MEDIA_FILENAME, file,
				NULL);
		else
			err = ca_context_play (
				mailnotification, 0,
				CA_PROP_EVENT_ID, "message-new-email",
				NULL);

		if (eca_debug) {
			if (err != 0 && file && *file)
				e_util_debug_print ("ECA", "Mail Notification: Failed to play '%s': %s\n", file, ca_strerror (err));
			else if (err != 0)
				e_util_debug_print ("ECA", "Mail Notification: Failed to play 'message-new-email' sound: %s\n", ca_strerror (err));
			else if (file && *file)
				e_util_debug_print ("ECA", "Mail Notification: Played file '%s'\n", file);
			else
				e_util_debug_print ("ECA", "Mail Notification: Played 'message-new-email' sound\n");
		}
#else
		if (eca_debug)
			e_util_debug_print ("ECA", "Mail Notification: Cannot play sound, not compiled with libcanberra\n");
#endif
	} else
		gdk_display_beep (gdk_display_get_default ());
}

struct _SoundConfigureWidgets {
	GtkToggleButton *enable;
	GtkToggleButton *beep;
	GtkToggleButton *use_theme;
	GtkFileChooser *filechooser;
};

static void
sound_file_set_cb (GtkFileChooser *file_chooser,
                   gpointer data)
{
	gchar *file;
	GSettings *settings;

	settings = e_util_ref_settings ("org.gnome.evolution.plugin.mail-notification");
	file = gtk_file_chooser_get_filename (file_chooser);

	g_settings_set_string (settings, CONF_KEY_SOUND_FILE, (file != NULL) ? file : "");

	g_object_unref (settings);
	g_free (file);
}

static void
sound_play_cb (GtkWidget *widget,
               struct _SoundConfigureWidgets *scw)
{
	gchar *file;

	if (!gtk_toggle_button_get_active (scw->enable))
		return;

	file = gtk_file_chooser_get_filename (scw->filechooser);

	do_play_sound (
		gtk_toggle_button_get_active (scw->beep),
		gtk_toggle_button_get_active (scw->use_theme),
		file);

	g_free (file);
}

struct _SoundNotifyData {
	time_t last_notify;
	guint notify_idle_id;
};

static gboolean
sound_notify_idle_cb (gpointer user_data)
{
	gchar *file;
	GSettings *settings;
	struct _SoundNotifyData *data = (struct _SoundNotifyData *) user_data;

	g_return_val_if_fail (data != NULL, FALSE);

	settings = e_util_ref_settings ("org.gnome.evolution.plugin.mail-notification");
	file = g_settings_get_string (settings, CONF_KEY_SOUND_FILE);

	do_play_sound (
		is_part_enabled (CONF_KEY_SOUND_BEEP),
		is_part_enabled (CONF_KEY_SOUND_USE_THEME),
		file);

	g_object_unref (settings);
	g_free (file);

	time (&data->last_notify);

	data->notify_idle_id = 0;

	return FALSE;
}

/* -------------------------------------------------------------------  */

static void
new_notify_sound (EMEventTargetFolder *t)
{
	time_t last_newmail;
	static struct _SoundNotifyData data = {0, 0};

	time (&last_newmail);

	/* just put it to the idle queue, if not under GNOME, where everything is
	   handled by the libnotify */
	if (data.notify_idle_id == 0 &&
	    (last_newmail - data.last_notify >= NOTIFY_THROTTLE) &&
	    !e_util_is_running_gnome ())
		data.notify_idle_id = g_idle_add_full (
			G_PRIORITY_LOW, sound_notify_idle_cb, &data, NULL);
}

static void
read_notify_sound (EMEventTargetMessage *t)
{
	/* we do nothing here */
}

static void
enable_sound (gint enable)
{
#ifdef HAVE_CANBERRA
	if (enable) {
		ca_context_create (&mailnotification);
		ca_context_change_props (
			mailnotification,
			CA_PROP_APPLICATION_NAME,
			"mailnotification Plugin",
			NULL);
	} else {
		ca_context_destroy (mailnotification);
		mailnotification = NULL;
	}
#endif
}

static GtkWidget *
get_config_widget_sound (void)
{
	GtkWidget *vbox;
	GtkWidget *container;
	GtkWidget *master;
	GtkWidget *widget;
	gchar *file;
	GSettings *settings;
	GSList *group = NULL;
	struct _SoundConfigureWidgets *scw;
	const gchar *text;

	scw = g_malloc0 (sizeof (struct _SoundConfigureWidgets));

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_show (vbox);

	container = vbox;

	text = _("_Play sound when a new message arrives");
	widget = gtk_check_button_new_with_mnemonic (text);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);

	settings = e_util_ref_settings ("org.gnome.evolution.plugin.mail-notification");

	g_settings_bind (
		settings, CONF_KEY_ENABLED_SOUND,
		widget, "active", G_SETTINGS_BIND_DEFAULT);

	master = widget;
	scw->enable = GTK_TOGGLE_BUTTON (widget);

	widget = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_start (widget, 12);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);

	e_binding_bind_property (
		master, "active",
		widget, "sensitive",
		G_BINDING_SYNC_CREATE);

	container = widget;

	text = _("_Beep");
	widget = gtk_radio_button_new_with_mnemonic (group, text);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);

	g_settings_bind (
		settings, CONF_KEY_SOUND_BEEP,
		widget, "active", G_SETTINGS_BIND_DEFAULT);

	scw->beep = GTK_TOGGLE_BUTTON (widget);

	group = gtk_radio_button_get_group (GTK_RADIO_BUTTON (widget));

	text = _("Use sound _theme");
	widget = gtk_radio_button_new_with_mnemonic (group, text);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);

	g_settings_bind (
		settings, CONF_KEY_SOUND_USE_THEME,
		widget, "active", G_SETTINGS_BIND_DEFAULT);

	scw->use_theme = GTK_TOGGLE_BUTTON (widget);

	group = gtk_radio_button_get_group (GTK_RADIO_BUTTON (widget));

	widget = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);

	container = widget;

	text = _("Play _file:");
	widget = gtk_radio_button_new_with_mnemonic (group, text);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);

	g_settings_bind (
		settings, CONF_KEY_SOUND_PLAY_FILE,
		widget, "active", G_SETTINGS_BIND_DEFAULT);

	text = _("Select sound file");
	widget = gtk_file_chooser_button_new (
		text, GTK_FILE_CHOOSER_ACTION_OPEN);
	gtk_box_pack_start (GTK_BOX (container), widget, TRUE, TRUE, 0);
	gtk_widget_show (widget);

	scw->filechooser = GTK_FILE_CHOOSER (widget);

	widget = gtk_button_new ();
	gtk_button_set_image (
		GTK_BUTTON (widget), gtk_image_new_from_icon_name (
		"media-playback-start", GTK_ICON_SIZE_BUTTON));
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);

	g_signal_connect (
		widget, "clicked",
		G_CALLBACK (sound_play_cb), scw);

	file = g_settings_get_string (settings, CONF_KEY_SOUND_FILE);

	if (file && *file)
		gtk_file_chooser_set_filename (scw->filechooser, file);

	g_object_unref (settings);
	g_free (file);

	g_signal_connect (
		scw->filechooser, "file-set",
		G_CALLBACK (sound_file_set_cb), scw);

	/* to let structure free properly */
	g_object_set_data_full (G_OBJECT (vbox), "scw-data", scw, g_free);

	return vbox;
}

/* -------------------------------------------------------------------  */
/*                     Plugin itself part                               */
/* -------------------------------------------------------------------  */

static void
e_mail_notif_open_gnome_notification_settings_cb (GtkWidget *button,
						  gpointer user_data)
{
#ifndef G_OS_WIN32
	GDesktopAppInfo *app_info;
	GError *error = NULL;

	app_info = g_desktop_app_info_new (GNOME_NOTIFICATIONS_PANEL_DESKTOP);

	g_return_if_fail (app_info != NULL);

	if (!g_app_info_launch (G_APP_INFO (app_info), NULL, NULL, &error)) {
		g_message ("%s: Failed with error: %s", G_STRFUNC, error ? error->message : "Unknown error");
	}

	g_clear_object (&app_info);
	g_clear_error (&error);
#endif
}

enum {
	E_MAIL_NOTIFY_ACCOUNTS_UID = 0,
	E_MAIL_NOTIFY_ACCOUNTS_DISPLAY_NAME,
	E_MAIL_NOTIFY_ACCOUNTS_ENABLED,
	E_MAIL_NOTIFY_ACCOUNTS_N_COLUMNS
};

static void
e_mail_notify_account_tree_view_enabled_toggled_cb (GtkCellRendererToggle *cell_renderer,
						    const gchar *path_string,
						    gpointer user_data)
{
	GtkTreeView *tree_view = user_data;
	GtkTreeModel *model;
	GtkTreePath *path;
	GtkTreeIter iter;
	GPtrArray *array;
	GSettings *settings;
	gboolean account_enabled = FALSE;

	g_return_if_fail (GTK_IS_TREE_VIEW (tree_view));

	model = gtk_tree_view_get_model (tree_view);
	path = gtk_tree_path_new_from_string (path_string);

	if (!gtk_tree_model_get_iter (model, &iter, path)) {
		gtk_tree_path_free (path);
		return;
	}

	gtk_tree_model_get (model, &iter, E_MAIL_NOTIFY_ACCOUNTS_ENABLED, &account_enabled, -1);
	gtk_list_store_set (GTK_LIST_STORE (model), &iter, E_MAIL_NOTIFY_ACCOUNTS_ENABLED, !account_enabled, -1);

	gtk_tree_path_free (path);

	array = g_ptr_array_new_with_free_func (g_free);

	if (gtk_tree_model_get_iter_first (model, &iter)) {
		do {
			gchar *uid = NULL;

			account_enabled = FALSE;

			gtk_tree_model_get (model, &iter,
				E_MAIL_NOTIFY_ACCOUNTS_ENABLED, &account_enabled,
				E_MAIL_NOTIFY_ACCOUNTS_UID, &uid,
				-1);

			if (!account_enabled && uid) {
				g_ptr_array_add (array, uid);
			} else {
				g_free (uid);
			}
		} while (gtk_tree_model_iter_next (model, &iter));
	}

	g_ptr_array_add (array, NULL);

	settings = e_util_ref_settings ("org.gnome.evolution.plugin.mail-notification");
	g_settings_set_strv (settings, "notify-not-accounts", (const gchar * const *) array->pdata);
	g_object_unref (settings);

	g_ptr_array_free (array, TRUE);
}

static GtkWidget *
get_config_widget_accounts (void)
{
	EShell *shell;
	GtkListStore *list_store;
	GtkTreeViewColumn *column;
	GtkCellRenderer *cell_renderer;
	GtkWidget *container;
	GtkWidget *tree_view;
	GtkWidget *widget;
	GtkWidget *label;

	widget = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	g_object_set (G_OBJECT (widget),
		"halign", GTK_ALIGN_FILL,
		"hexpand", TRUE,
		"valign", GTK_ALIGN_FILL,
		"vexpand", TRUE,
		"border-width", 12,
		NULL);

	container = widget;

	widget = gtk_label_new_with_mnemonic (_("Select _accounts for which enable notifications:"));
	g_object_set (G_OBJECT (widget),
		"halign", GTK_ALIGN_START,
		"hexpand", FALSE,
		"valign", GTK_ALIGN_CENTER,
		"vexpand", FALSE,
		NULL);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	label = widget;

	widget = gtk_scrolled_window_new (NULL, NULL);
	g_object_set (G_OBJECT (widget),
		"halign", GTK_ALIGN_FILL,
		"hexpand", TRUE,
		"valign", GTK_ALIGN_FILL,
		"vexpand", TRUE,
		"hscrollbar-policy", GTK_POLICY_AUTOMATIC,
		"vscrollbar-policy", GTK_POLICY_AUTOMATIC,
		"shadow-type", GTK_SHADOW_IN,
		NULL);

	gtk_box_pack_start (GTK_BOX (container), widget, TRUE, TRUE, 0);

	list_store = gtk_list_store_new (E_MAIL_NOTIFY_ACCOUNTS_N_COLUMNS,
		G_TYPE_STRING,
		G_TYPE_STRING,
		G_TYPE_BOOLEAN);

	shell = e_shell_get_default ();
	g_warn_if_fail (shell != NULL);

	if (shell) {
		EMailAccountStore *account_store = NULL;
		EShellBackend *shell_backend;

		shell_backend = e_shell_get_backend_by_name (shell, "mail");
		if (shell_backend) {
			EMailSession *mail_session;

			mail_session = e_mail_backend_get_session (E_MAIL_BACKEND (shell_backend));
			account_store = e_mail_ui_session_get_account_store (E_MAIL_UI_SESSION (mail_session));
		}

		if (account_store) {
			GSettings *settings;
			GtkTreeModel *amodel = GTK_TREE_MODEL (account_store);
			GtkTreeIter aiter;
			gchar **strv;
			GHashTable *local_not_accounts;
			gint ii;

			settings = e_util_ref_settings ("org.gnome.evolution.plugin.mail-notification");
			strv = g_settings_get_strv (settings, "notify-not-accounts");
			g_object_unref (settings);

			/* Borrows values from 'strv', thus free 'strv' only after free of 'local_not_accounts' */
			local_not_accounts = g_hash_table_new (g_str_hash, g_str_equal);

			for (ii = 0; strv && strv[ii]; ii++) {
				g_hash_table_insert (local_not_accounts, strv[ii], NULL);
			}

			if (gtk_tree_model_get_iter_first (amodel, &aiter)) {
				do {
					CamelService *service = NULL;

					gtk_tree_model_get (amodel, &aiter,
						E_MAIL_ACCOUNT_STORE_COLUMN_SERVICE, &service,
						-1);

					if (service) {
						GtkTreeIter iter;
						const gchar *uid;

						uid = camel_service_get_uid (service);

						if (g_strcmp0 (uid, E_MAIL_SESSION_VFOLDER_UID) != 0) {
							gtk_list_store_append (list_store, &iter);
							gtk_list_store_set (list_store, &iter,
								E_MAIL_NOTIFY_ACCOUNTS_UID, uid,
								E_MAIL_NOTIFY_ACCOUNTS_DISPLAY_NAME, camel_service_get_display_name (service),
								E_MAIL_NOTIFY_ACCOUNTS_ENABLED, !g_hash_table_contains (local_not_accounts, uid),
								-1);
						}
					}

					g_clear_object (&service);
				} while (gtk_tree_model_iter_next (amodel, &aiter));
			}

			g_hash_table_destroy (local_not_accounts);
			g_strfreev (strv);
		}
	}

	tree_view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (list_store));
	g_object_set (G_OBJECT (tree_view),
		"halign", GTK_ALIGN_FILL,
		"hexpand", TRUE,
		"valign", GTK_ALIGN_FILL,
		"vexpand", TRUE,
		NULL);

	g_object_unref (list_store);

	gtk_container_add (GTK_CONTAINER (widget), tree_view);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), tree_view);

	column = gtk_tree_view_column_new ();
	gtk_tree_view_column_set_expand (column, FALSE);
	gtk_tree_view_column_set_title (column, _("Enabled"));

	cell_renderer = gtk_cell_renderer_toggle_new ();
	gtk_tree_view_column_pack_start (column, cell_renderer, TRUE);

	g_signal_connect (
		cell_renderer, "toggled",
		G_CALLBACK (e_mail_notify_account_tree_view_enabled_toggled_cb),
		tree_view);

	gtk_tree_view_column_add_attribute (column, cell_renderer, "active", E_MAIL_NOTIFY_ACCOUNTS_ENABLED);

	gtk_tree_view_append_column (GTK_TREE_VIEW (tree_view), column);

	column = gtk_tree_view_column_new ();
	gtk_tree_view_column_set_expand (column, TRUE);
	gtk_tree_view_column_set_title (column, _("Account Name"));

	cell_renderer = gtk_cell_renderer_text_new ();
	g_object_set (cell_renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	gtk_tree_view_column_pack_start (column, cell_renderer, FALSE);

	gtk_tree_view_column_add_attribute (column, cell_renderer, "text", E_MAIL_NOTIFY_ACCOUNTS_DISPLAY_NAME);

	gtk_tree_view_append_column (GTK_TREE_VIEW (tree_view), column);

	gtk_widget_show_all (container);

	return container;
}

static GtkWidget *
get_cfg_widget (void)
{
	GtkWidget *container;
	GtkWidget *notebook;
	GtkWidget *widget;
	GSettings *settings;
	const gchar *text;
	gchar *tmp;

	settings = e_util_ref_settings ("org.gnome.evolution.plugin.mail-notification");

	notebook = gtk_notebook_new ();
	gtk_widget_show (notebook);

	widget = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
	gtk_container_set_border_width (GTK_CONTAINER (widget), 12);
	gtk_widget_show (widget);

	container = widget;

	tmp = g_strconcat ("<b>", _("Mail Notification"), "</b>", NULL);
	widget = gtk_label_new ("");
	g_object_set (G_OBJECT (widget),
		"halign", GTK_ALIGN_START,
		"hexpand", FALSE,
		"valign", GTK_ALIGN_CENTER,
		"vexpand", FALSE,
		"use-markup", TRUE,
		"label", tmp,
		NULL);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);
	g_free (tmp);

	text = _("Notify new messages for _Inbox only");
	widget = gtk_check_button_new_with_mnemonic (text);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);

	g_settings_bind (
		settings, CONF_KEY_NOTIFY_ONLY_INBOX,
		widget, "active", G_SETTINGS_BIND_DEFAULT);

	if (e_util_is_running_gnome ()) {
		widget = gtk_button_new_with_mnemonic ("Open _GNOME Notification settings");
		g_signal_connect (widget, "clicked", G_CALLBACK (e_mail_notif_open_gnome_notification_settings_cb), NULL);
		gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
		gtk_widget_show (widget);
	} else {
#ifdef HAVE_LIBNOTIFY
		text = _("Show _notification when a new message arrives");
		widget = gtk_check_button_new_with_mnemonic (text);
		gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
		gtk_widget_show (widget);

		g_settings_bind (
			settings, CONF_KEY_ENABLED_STATUS,
			widget, "active", G_SETTINGS_BIND_DEFAULT);
#endif

		widget = get_config_widget_sound ();
		gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	}

	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), container, gtk_label_new (_("Configuration")));

	widget = get_config_widget_accounts ();

	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), widget, gtk_label_new (_("Accounts")));

	g_object_unref (settings);

	return notebook;
}

void org_gnome_mail_new_notify (EPlugin *ep, EMEventTargetFolder *t);
void org_gnome_mail_unread_notify (EPlugin *ep, EMEventTargetFolderUnread *t);
void org_gnome_mail_read_notify (EPlugin *ep, EMEventTargetMessage *t);

gint e_plugin_lib_enable (EPlugin *ep, gint enable);
GtkWidget *e_plugin_lib_get_configure_widget (EPlugin *epl);

void
org_gnome_mail_new_notify (EPlugin *ep,
                           EMEventTargetFolder *t)
{
	g_return_if_fail (t != NULL);

	if (!enabled || !t->new ||
	    (!t->is_inbox && is_part_enabled (CONF_KEY_NOTIFY_ONLY_INBOX)) ||
	    !can_notify_account (t->store))
		return;

	g_mutex_lock (&mlock);

	new_notify_dbus (t);

#ifdef HAVE_LIBNOTIFY
	if (is_part_enabled (CONF_KEY_ENABLED_STATUS) || e_util_is_running_gnome ())
		new_notify_status (t);
#endif

	if (is_part_enabled (CONF_KEY_ENABLED_SOUND))
		new_notify_sound (t);

	g_mutex_unlock (&mlock);
}

void
org_gnome_mail_unread_notify (EPlugin *ep,
                              EMEventTargetFolderUnread *t)
{
#ifdef HAVE_LIBNOTIFY
	g_return_if_fail (t != NULL);

	if (!enabled ||
	    (!t->is_inbox && is_part_enabled (CONF_KEY_NOTIFY_ONLY_INBOX)) ||
	    !can_notify_account (t->store))
		return;

	g_mutex_lock (&mlock);

	if (is_part_enabled (CONF_KEY_ENABLED_STATUS) || e_util_is_running_gnome ())
		unread_notify_status (t);

	g_mutex_unlock (&mlock);
#endif
}

void
org_gnome_mail_read_notify (EPlugin *ep,
                            EMEventTargetMessage *t)
{
	g_return_if_fail (t != NULL);

	if (!enabled || !can_notify_account (camel_folder_get_parent_store (t->folder)))
		return;

	g_mutex_lock (&mlock);

	read_notify_dbus (t);

#ifdef HAVE_LIBNOTIFY
	if (is_part_enabled (CONF_KEY_ENABLED_STATUS) || e_util_is_running_gnome ())
		read_notify_status (t);
#endif

	if (is_part_enabled (CONF_KEY_ENABLED_SOUND))
		read_notify_sound (t);

	g_mutex_unlock (&mlock);
}

gint
e_plugin_lib_enable (EPlugin *ep,
                     gint enable)
{
	if (enable) {
		enable_dbus (enable);

		if (is_part_enabled (CONF_KEY_ENABLED_SOUND))
			enable_sound (enable);

		g_mutex_lock (&mlock);

		if (!not_accounts_handler_id) {
			GSettings *settings;

			settings = e_util_ref_settings ("org.gnome.evolution.plugin.mail-notification");
			mail_notify_not_accounts_changed_locked (settings);
			not_accounts_handler_id = g_signal_connect (settings, "changed::notify-not-accounts",
				G_CALLBACK (mail_notify_not_accounts_changed_cb), NULL);
			g_object_unref (settings);
		}

		g_mutex_unlock (&mlock);

		enabled = TRUE;
	} else {
		enable_dbus (enable);
		enable_sound (enable);

#ifdef HAVE_LIBNOTIFY
		remove_notification ();
#endif

		g_mutex_lock (&mlock);

		if (not_accounts_handler_id) {
			GSettings *settings;

			settings = e_util_ref_settings ("org.gnome.evolution.plugin.mail-notification");
			g_signal_handler_disconnect (settings, not_accounts_handler_id);
			g_object_unref (settings);

			not_accounts_handler_id = 0;

			g_clear_pointer (&not_accounts, g_hash_table_destroy);
		}

		g_mutex_unlock (&mlock);

		enabled = FALSE;
	}

	return 0;
}

GtkWidget *
e_plugin_lib_get_configure_widget (EPlugin *epl)
{
	return get_cfg_widget ();
}
