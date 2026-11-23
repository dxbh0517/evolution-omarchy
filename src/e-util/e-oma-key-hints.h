/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Holding Ctrl shows the keyboard shortcut of every visible action
 * button as a small badge over the window.
 */

#if !defined (__E_UTIL_H_INSIDE__) && !defined (LIBEUTIL_COMPILATION)
#error "Only <e-util/e-util.h> should be included directly."
#endif

#ifndef E_OMA_KEY_HINTS_H
#define E_OMA_KEY_HINTS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Data key on an EUIAction holding an accelerator string to show when the
 * action itself has none, but the shortcut is reached through another
 * action (e.g. Ctrl+F revealing the search bar). */
#define E_OMA_KEY_HINT_ACCEL "oma-key-hint-accel"

void		e_oma_key_hints_attach		(GtkWindow *window,
						 GtkOverlay *overlay);

G_END_DECLS

#endif /* E_OMA_KEY_HINTS_H */
