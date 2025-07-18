/*
 *      notebook.c - this file is part of Geany, a fast and lightweight IDE
 *
 *      Copyright 2006 The Geany contributors
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License along
 *      with this program; if not, write to the Free Software Foundation, Inc.,
 *      51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Notebook tab Drag 'n' Drop reordering and tab management.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "notebook.h"

#include "callbacks.h"
#include "documentprivate.h"
#include "geanyobject.h"
#include "keybindings.h"
#include "main.h"
#include "support.h"
#include "ui_utils.h"
#include "utils.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>


#define GEANY_DND_NOTEBOOK_TAB_TYPE	"geany_dnd_notebook_tab"

static const GtkTargetEntry drag_targets[] =
{
	{ (gchar *) GEANY_DND_NOTEBOOK_TAB_TYPE, GTK_TARGET_SAME_APP | GTK_TARGET_SAME_WIDGET, 0 }
};

static const GtkTargetEntry files_drop_targets[] = {
	{ (gchar *) "STRING",			0, 0 },
	{ (gchar *) "UTF8_STRING",		0, 0 },
	{ (gchar *) "text/plain",		0, 0 },
	{ (gchar *) "text/uri-list",	0, 0 }
};

static const gsize MAX_MRU_DOCS = 20;
static GQueue *mru_docs = NULL;
static guint mru_pos = 0;

static gboolean switch_in_progress = FALSE;
static GtkWidget *switch_dialog = NULL;
static GtkWidget *switch_dialog_label = NULL;


static void
notebook_page_reordered_cb(GtkNotebook *notebook, GtkWidget *child, guint page_num,
		gpointer user_data);

static void
on_window_drag_data_received(GtkWidget *widget, GdkDragContext *drag_context,
		gint x, gint y, GtkSelectionData *data, guint target_type,
		guint event_time, gpointer user_data);

static void
notebook_tab_close_clicked_cb(GtkButton *button, gpointer user_data);

static void setup_tab_dnd(void);


static void update_mru_docs_head(GeanyDocument *doc)
{
	if (doc)
	{
		g_queue_remove(mru_docs, doc);
		g_queue_push_head(mru_docs, doc);

		if (g_queue_get_length(mru_docs) > MAX_MRU_DOCS)
			g_queue_pop_tail(mru_docs);
	}
}


/* before the tab changes, add the current document to the MRU list */
static void on_notebook_switch_page(GtkNotebook *notebook,
	gpointer page, guint page_num, gpointer user_data)
{
	GeanyDocument *new;

	new = document_get_from_page(page_num);

	/* insert the very first document (when adding the second document
	 * and switching to it) */
	if (g_queue_get_length(mru_docs) == 0 && gtk_notebook_get_n_pages(notebook) == 2)
		update_mru_docs_head(document_get_current());

	if (!switch_in_progress)
		update_mru_docs_head(new);
}


static void on_document_close(GObject *obj, GeanyDocument *doc)
{
	if (! main_status.quitting)
	{
		g_queue_remove(mru_docs, doc);
		/* this prevents the pop up window from showing when there's a single
		 * document */
		if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(main_widgets.notebook)) == 2)
			g_queue_clear(mru_docs);
	}
}


static GtkWidget *ui_minimal_dialog_new(GtkWindow *parent, const gchar *title)
{
	GtkWidget *dialog;

	dialog = gtk_window_new(GTK_WINDOW_POPUP);

	if (parent)
	{
		gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
		gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
	}
	gtk_window_set_title(GTK_WINDOW(dialog), title);
	gtk_window_set_type_hint(GTK_WINDOW(dialog), GDK_WINDOW_TYPE_HINT_DIALOG);
	gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);

	gtk_widget_set_name(dialog, "GeanyDialog");
	return dialog;
}


static gboolean is_modifier_key(guint keyval)
{
	switch (keyval)
	{
		case GDK_KEY_Shift_L:
		case GDK_KEY_Shift_R:
		case GDK_KEY_Control_L:
		case GDK_KEY_Control_R:
		case GDK_KEY_Meta_L:
		case GDK_KEY_Meta_R:
		case GDK_KEY_Alt_L:
		case GDK_KEY_Alt_R:
		case GDK_KEY_Super_L:
		case GDK_KEY_Super_R:
		case GDK_KEY_Hyper_L:
		case GDK_KEY_Hyper_R:
			return TRUE;
		default:
			return FALSE;
	}
}


static gboolean on_key_release_event(GtkWidget *widget, GdkEventKey *ev, gpointer user_data)
{
	/* user may have rebound keybinding to a different modifier than Ctrl, so check all */
	if (switch_in_progress && is_modifier_key(ev->keyval))
	{
		GeanyDocument *doc;

		switch_in_progress = FALSE;

		if (switch_dialog)
		{
			gtk_widget_destroy(switch_dialog);
			switch_dialog = NULL;
		}

		doc = document_get_current();
		update_mru_docs_head(doc);
		mru_pos = 0;
		document_check_disk_status(doc, TRUE);
	}
	return FALSE;
}


static GtkWidget *create_switch_dialog(void)
{
	GtkWidget *dialog, *widget, *vbox;

	dialog = ui_minimal_dialog_new(GTK_WINDOW(main_widgets.window), _("Switch to Document"));
	gtk_window_set_decorated(GTK_WINDOW(dialog), FALSE);
	gtk_window_set_default_size(GTK_WINDOW(dialog), 200, -1);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
	gtk_container_add(GTK_CONTAINER(dialog), vbox);

	widget = gtk_image_new_from_stock(GTK_STOCK_JUMP_TO, GTK_ICON_SIZE_BUTTON);
	gtk_container_add(GTK_CONTAINER(vbox), widget);

	widget = gtk_label_new(NULL);
	gtk_label_set_justify(GTK_LABEL(widget), GTK_JUSTIFY_CENTER);
	gtk_container_add(GTK_CONTAINER(vbox), widget);
	switch_dialog_label = widget;

	g_signal_connect(dialog, "key-release-event", G_CALLBACK(on_key_release_event), NULL);
	return dialog;
}


static void update_filename_label(void)
{
	guint i;
	guint queue_length;
	GeanyDocument *doc;
	GString *markup = g_string_new(NULL);

	if (!switch_dialog)
	{
		switch_dialog = create_switch_dialog();
		gtk_widget_show_all(switch_dialog);
	}

	queue_length = g_queue_get_length(mru_docs);
	for (i = mru_pos; (i <= mru_pos + 3) && (doc = g_queue_peek_nth(mru_docs, i % queue_length)); i++)
	{
		gchar *basename;

		basename = g_path_get_basename(DOC_FILENAME(doc));
		SETPTR(basename, g_markup_escape_text(basename, -1));

		if (i == mru_pos)
			g_string_printf(markup, "<b>%s</b>", basename);
		else if (i % queue_length == mru_pos)    /* && i != mru_pos */
		{
			/* We have wrapped around and got to the starting document again */
			g_free(basename);
			break;
		}
		else
		{
			g_string_append(markup, "\n");
			if (doc->changed)
				SETPTR(basename, g_strconcat("<span color='red'>", basename, "</span>", NULL));
			g_string_append(markup, basename);
		}
		g_free(basename);
	}
	gtk_label_set_markup(GTK_LABEL(switch_dialog_label), markup->str);
	g_string_free(markup, TRUE);
}


static gboolean on_switch_timeout(G_GNUC_UNUSED gpointer data)
{
	if (!switch_in_progress || switch_dialog)
	{
		return FALSE;
	}

	update_filename_label();
	return FALSE;
}


void notebook_switch_tablastused(void)
{
	GeanyDocument *last_doc;
	gboolean switch_start = !switch_in_progress;

	mru_pos += 1;
	last_doc = g_queue_peek_nth(mru_docs, mru_pos);

	if (! DOC_VALID(last_doc))
	{
		utils_beep();
		mru_pos = 0;
		last_doc = g_queue_peek_nth(mru_docs, mru_pos);
	}
	if (! DOC_VALID(last_doc))
		return;

	switch_in_progress = TRUE;
	document_show_tab(last_doc);

	/* if there's a modifier key, we can switch back in MRU order each time unless
	 * the key is released */
	if (switch_start)
		g_timeout_add(600, on_switch_timeout, NULL);
	else
		update_filename_label();
}


gboolean notebook_switch_in_progress(void)
{
	return switch_in_progress;
}


static gboolean focus_sci(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	GeanyDocument *doc = document_get_current();

	if (doc != NULL && event->button == 1)
		gtk_widget_grab_focus(GTK_WIDGET(doc->editor->sci));

	return FALSE;
}


static gboolean gtk_notebook_show_arrows(GtkNotebook *notebook)
{
	return gtk_notebook_get_scrollable(notebook);
#if 0
	/* To get this working we would need to define at least the first two fields of
	 * GtkNotebookPage since it is a private field. The better way would be to
	 * subclass GtkNotebook.
struct _FakeGtkNotebookPage
{
	GtkWidget *child;
	GtkWidget *tab_label;
};
 */
	gboolean show_arrow = FALSE;
	GList *children;

	if (! notebook->scrollable)
		return FALSE;

	children = notebook->children;
	while (children)
	{
		struct _FakeGtkNotebookPage *page = children->data;

		if (page->tab_label && ! gtk_widget_get_child_visible(page->tab_label))
			show_arrow = TRUE;

		children = children->next;
	}
	return show_arrow;
#endif
}


static gboolean is_position_on_tab_bar(GtkNotebook *notebook, GdkEventButton *event)
{
	GtkWidget *page;
	GtkWidget *tab;
	GtkWidget *nb;
	GtkPositionType tab_pos;
	gint scroll_arrow_hlength, scroll_arrow_vlength;
	gdouble x, y;

	page = gtk_notebook_get_nth_page(notebook, 0);
	g_return_val_if_fail(page != NULL, FALSE);

	tab = gtk_notebook_get_tab_label(notebook, page);
	g_return_val_if_fail(tab != NULL, FALSE);

	tab_pos = gtk_notebook_get_tab_pos(notebook);
	nb = GTK_WIDGET(notebook);

	gtk_widget_style_get(GTK_WIDGET(notebook), "scroll-arrow-hlength", &scroll_arrow_hlength,
		"scroll-arrow-vlength", &scroll_arrow_vlength, NULL);

	if (! gdk_event_get_coords((GdkEvent*) event, &x, &y))
	{
		x = event->x;
		y = event->y;
	}

	switch (tab_pos)
	{
		case GTK_POS_TOP:
		case GTK_POS_BOTTOM:
		{
			if (event->y >= 0 && event->y <= gtk_widget_get_allocated_height(tab))
			{
				if (! gtk_notebook_show_arrows(notebook) || (
					x > scroll_arrow_hlength &&
					x < gtk_widget_get_allocated_width(nb) - scroll_arrow_hlength))
					return TRUE;
			}
			break;
		}
		case GTK_POS_LEFT:
		case GTK_POS_RIGHT:
		{
			if (event->x >= 0 && event->x <= gtk_widget_get_allocated_width(tab))
			{
				if (! gtk_notebook_show_arrows(notebook) || (
					y > scroll_arrow_vlength &&
					y < gtk_widget_get_allocated_height(nb) - scroll_arrow_vlength))
					return TRUE;
			}
		}
	}

	return FALSE;
}


static void tab_bar_menu_activate_cb(GtkMenuItem *menuitem, gpointer data)
{
	GeanyDocument *doc = data;

	if (! DOC_VALID(doc))
		return;

	document_show_tab(doc);
}


static void on_open_in_new_window_activate(GtkMenuItem *menuitem, gpointer user_data)
{
	GeanyDocument *doc = user_data;
	gchar *doc_path;

	g_return_if_fail(doc->is_valid);

	doc_path = utils_get_locale_from_utf8(doc->file_name);
	utils_start_new_geany_instance(doc_path);
	g_free(doc_path);
}


static gboolean has_tabs_on_right(GeanyDocument *doc)
{
	GtkNotebook *nb = GTK_NOTEBOOK(main_widgets.notebook);
	gint total_pages = gtk_notebook_get_n_pages(nb);
	gint doc_page = document_get_notebook_page(doc);
	return total_pages > (doc_page + 1);
}


static void on_close_documents_right_activate(GtkMenuItem *menuitem, GeanyDocument *doc)
{
	g_return_if_fail(has_tabs_on_right(doc));
	GtkNotebook *nb = GTK_NOTEBOOK(main_widgets.notebook);
	gint current_page = gtk_notebook_get_current_page(nb);
	gint doc_page = document_get_notebook_page(doc);
	for (gint i = doc_page + 1; i < gtk_notebook_get_n_pages(nb); )
	{
		if (! document_close(document_get_from_page(i)))
			i++; // only increment if tab wasn't closed
	}
	/* keep the current tab to the original one unless it has been closed, in
	 * which case use the activated one */
	gtk_notebook_set_current_page(nb, MIN(current_page, doc_page));
}


static void show_tab_bar_popup_menu(GdkEventButton *event, GeanyDocument *doc)
{
	GtkWidget *menu_item;
	static GtkWidget *menu = NULL;

	if (menu == NULL)
		menu = gtk_menu_new();

	/* clear the old menu items */
	gtk_container_foreach(GTK_CONTAINER(menu), (GtkCallback) (void(*)(void)) gtk_widget_destroy, NULL);

	ui_menu_add_document_items(GTK_MENU(menu), document_get_current(),
		G_CALLBACK(tab_bar_menu_activate_cb));

	menu_item = gtk_separator_menu_item_new();
	gtk_widget_show(menu_item);
	gtk_container_add(GTK_CONTAINER(menu), menu_item);

	menu_item = ui_image_menu_item_new(GTK_STOCK_OPEN, _("Open in New _Window"));
	gtk_widget_show(menu_item);
	gtk_container_add(GTK_CONTAINER(menu), menu_item);
	g_signal_connect(menu_item, "activate",
		G_CALLBACK(on_open_in_new_window_activate), doc);
	/* disable if not on disk */
	if (doc == NULL || !doc->real_path)
		gtk_widget_set_sensitive(menu_item, FALSE);

	menu_item = gtk_separator_menu_item_new();
	gtk_widget_show(menu_item);
	gtk_container_add(GTK_CONTAINER(menu), menu_item);

	menu_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_CLOSE, NULL);
	gtk_widget_show(menu_item);
	gtk_container_add(GTK_CONTAINER(menu), menu_item);
	g_signal_connect(menu_item, "activate", G_CALLBACK(notebook_tab_close_clicked_cb), doc);
	gtk_widget_set_sensitive(GTK_WIDGET(menu_item), (doc != NULL));

	menu_item = ui_image_menu_item_new(GTK_STOCK_CLOSE, _("Close Ot_her Documents"));
	gtk_widget_show(menu_item);
	gtk_container_add(GTK_CONTAINER(menu), menu_item);
	g_signal_connect(menu_item, "activate", G_CALLBACK(on_close_other_documents1_activate), doc);
	gtk_widget_set_sensitive(GTK_WIDGET(menu_item), (doc != NULL));

	menu_item = ui_image_menu_item_new(GTK_STOCK_CLOSE, _("Close Documents to the _Right"));
	gtk_widget_show(menu_item);
	gtk_container_add(GTK_CONTAINER(menu), menu_item);
	g_signal_connect(menu_item, "activate", G_CALLBACK(on_close_documents_right_activate), doc);
	gtk_widget_set_sensitive(GTK_WIDGET(menu_item), doc != NULL && has_tabs_on_right(doc));

	menu_item = ui_image_menu_item_new(GTK_STOCK_CLOSE, _("C_lose All"));
	gtk_widget_show(menu_item);
	gtk_container_add(GTK_CONTAINER(menu), menu_item);
	g_signal_connect(menu_item, "activate", G_CALLBACK(on_close_all1_activate), NULL);

	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *) event);
}


static gboolean notebook_tab_bar_click_cb(GtkWidget *widget, GdkEventButton *event,
										  gpointer user_data)
{
	if (event->type == GDK_2BUTTON_PRESS)
	{
		GtkNotebook *notebook = GTK_NOTEBOOK(widget);
		GtkWidget *event_widget = gtk_get_event_widget((GdkEvent *) event);
		GtkWidget *child = gtk_notebook_get_nth_page(notebook, gtk_notebook_get_current_page(notebook));

		/* ignore events from the content of the page (impl. stolen from GTK2 tab scrolling)
		 * TODO: we should also ignore notebook's action widgets, but that's more work and
		 * we don't have any of them yet anyway -- and GTK 2.16 don't have those actions. */
		if (event_widget == NULL || event_widget == child || gtk_widget_is_ancestor(event_widget, child))
			return FALSE;

		if (is_position_on_tab_bar(notebook, event))
		{
			document_new_file(NULL, NULL, NULL);
			return TRUE;
		}
	}
	/* right-click is also handled here if it happened on the notebook tab bar but not
	 * on a tab directly */
	else if (event->button == 3)
	{
		show_tab_bar_popup_menu(event, NULL);
		return TRUE;
	}
	return FALSE;
}


static gboolean notebook_tab_bar_scroll_cb(GtkWidget *widget, GdkEventScroll *event)
{
	GtkNotebook *notebook = GTK_NOTEBOOK(widget);
	GtkWidget *child;

	child = gtk_notebook_get_nth_page(notebook, gtk_notebook_get_current_page(notebook));
	if (child == NULL)
		return FALSE;

	switch (event->direction)
	{
		case GDK_SCROLL_RIGHT:
		case GDK_SCROLL_DOWN:
			gtk_notebook_next_page(notebook);
			break;
		case GDK_SCROLL_LEFT:
		case GDK_SCROLL_UP:
			gtk_notebook_prev_page(notebook);
			break;
		default:
			break;
	}

	return TRUE;
}


void notebook_init(void)
{
	g_signal_connect_after(main_widgets.notebook, "button-press-event",
		G_CALLBACK(notebook_tab_bar_click_cb), NULL);

	g_signal_connect(main_widgets.notebook, "drag-data-received",
		G_CALLBACK(on_window_drag_data_received), NULL);

	mru_docs = g_queue_new();
	g_signal_connect(main_widgets.notebook, "switch-page",
		G_CALLBACK(on_notebook_switch_page), NULL);
	g_signal_connect(geany_object, "document-close",
		G_CALLBACK(on_document_close), NULL);

	gtk_widget_add_events(main_widgets.notebook, GDK_SCROLL_MASK);
	g_signal_connect(main_widgets.notebook, "scroll-event", G_CALLBACK(notebook_tab_bar_scroll_cb), NULL);

	/* in case the switch dialog misses an event while drawing the dialog */
	g_signal_connect(main_widgets.window, "key-release-event", G_CALLBACK(on_key_release_event), NULL);

	setup_tab_dnd();
}


void notebook_free(void)
{
	g_queue_free(mru_docs);
}


static void setup_tab_dnd(void)
{
	GtkWidget *notebook = main_widgets.notebook;

	g_signal_connect(notebook, "page-reordered", G_CALLBACK(notebook_page_reordered_cb), NULL);
}


static void
notebook_page_reordered_cb(GtkNotebook *notebook, GtkWidget *child, guint page_num,
	gpointer user_data)
{
	/* Not necessary to update open files treeview if it's sorted.
	 * Note: if enabled, it's best to move the item instead of recreating all items. */
	/*sidebar_openfiles_update_all();*/
}


/* call this after the number of tabs in main_widgets.notebook changes. */
static void tab_count_changed(void)
{
	switch (gtk_notebook_get_n_pages(GTK_NOTEBOOK(main_widgets.notebook)))
	{
		case 0:
		/* Enables DnD for dropping files into the empty notebook widget */
		gtk_drag_dest_set(main_widgets.notebook, GTK_DEST_DEFAULT_ALL,
			files_drop_targets,	G_N_ELEMENTS(files_drop_targets),
			GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK | GDK_ACTION_ASK);
		break;

		case 1:
		/* Disables DnD for dropping files into the notebook widget and enables the DnD for moving file
		 * tabs. Files can still be dropped into the notebook widget because it will be handled by the
		 * active Scintilla Widget (only dropping to the tab bar is not possible but it should be ok) */
		gtk_drag_dest_set(main_widgets.notebook, GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_DROP,
			drag_targets, G_N_ELEMENTS(drag_targets), GDK_ACTION_MOVE);
		break;
	}
}


static gboolean notebook_tab_click(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	guint state;
	GeanyDocument *doc = (GeanyDocument *) data;

	/* toggle additional widgets on double click */
	if (event->type == GDK_2BUTTON_PRESS)
	{
		if (interface_prefs.notebook_double_click_hides_widgets)
			on_menu_toggle_all_additional_widgets1_activate(NULL, NULL);

		return TRUE; /* stop other handlers like notebook_tab_bar_click_cb() */
	}
	/* close tab on middle click */
	if (event->button == 2)
	{
		document_close(doc);
		return TRUE; /* stop other handlers like notebook_tab_bar_click_cb() */
	}
	/* switch last used tab on ctrl-click */
	state = keybindings_get_modifiers(event->state);
	if (event->button == 1 && state == GEANY_PRIMARY_MOD_MASK)
	{
		keybindings_send_command(GEANY_KEY_GROUP_NOTEBOOK,
			GEANY_KEYS_NOTEBOOK_SWITCHTABLASTUSED);
		return TRUE;
	}
	/* right-click is first handled here if it happened on a notebook tab */
	if (event->button == 3)
	{
		show_tab_bar_popup_menu(event, doc);
		return TRUE;
	}

	return FALSE;
}


static void notebook_tab_close_button_style_set(GtkWidget *btn, GtkRcStyle *prev_style,
												gpointer data)
{
	gint w, h;

	gtk_icon_size_lookup_for_settings(gtk_widget_get_settings(btn), GTK_ICON_SIZE_MENU, &w, &h);
	gtk_widget_set_size_request(btn, w + 2, h + 2);
}


/* Returns page number of notebook page, or -1 on error
 *
 * Note: the widget added to the notebook is *not* shown by this function, so you have to call
 * something like `gtk_widget_show(document_get_notebook_child(doc))` when finished setting up the
 * document.  This is necessary because when the notebook tab is added, the document isn't ready
 * yet, and we  need the notebook to emit ::switch-page after it actually is.  Actually this
 * doesn't prevent the signal to me emitted straight when we insert the page (this looks like a
 * GTK bug), but it emits it again when showing the child, and it's all we need. */
gint notebook_new_tab(GeanyDocument *this)
{
	GtkWidget *hbox, *ebox, *vbox;
	gint tabnum;
	GtkWidget *page;
	gint cur_page;

	g_return_val_if_fail(this != NULL, -1);

	/* page is packed into a vbox so we can stack infobars above it */
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	page = GTK_WIDGET(this->editor->sci);
	gtk_box_pack_start(GTK_BOX(vbox), page, TRUE, TRUE, 0);

	this->priv->tab_label = gtk_label_new(NULL);

	/* get button press events for the tab label and the space between it and
	 * the close button, if any */
	ebox = gtk_event_box_new();
	gtk_widget_set_has_window(ebox, FALSE);
	g_signal_connect(ebox, "button-press-event", G_CALLBACK(notebook_tab_click), this);
	/* focus the current document after clicking on a tab */
	g_signal_connect_after(ebox, "button-release-event",
		G_CALLBACK(focus_sci), NULL);

    /* switch tab by scrolling - GTK2 behaviour for GTK3 */
	gtk_widget_add_events(GTK_WIDGET(ebox), GDK_SCROLL_MASK);
	gtk_widget_add_events(GTK_WIDGET(this->priv->tab_label), GDK_SCROLL_MASK);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_box_pack_start(GTK_BOX(hbox), this->priv->tab_label, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(ebox), hbox);

	if (file_prefs.show_tab_cross)
	{
		GtkWidget *image, *btn, *align;

		btn = gtk_button_new();
		gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
		gtk_button_set_focus_on_click(GTK_BUTTON(btn), FALSE);
		gtk_widget_set_name(btn, "geany-close-tab-button");

		image = gtk_image_new_from_stock(GTK_STOCK_CLOSE, GTK_ICON_SIZE_MENU);
		gtk_container_add(GTK_CONTAINER(btn), image);

		align = gtk_alignment_new(1.0, 0.5, 0.0, 0.0);
		gtk_container_add(GTK_CONTAINER(align), btn);
		gtk_box_pack_start(GTK_BOX(hbox), align, TRUE, TRUE, 0);

		g_signal_connect(btn, "clicked", G_CALLBACK(notebook_tab_close_clicked_cb), this);
		/* button overrides event box, so make middle click on button also close tab */
		g_signal_connect(btn, "button-press-event", G_CALLBACK(notebook_tab_click), this);
		/* handle style modification to keep button small as possible even when theme change */
		g_signal_connect(btn, "style-set", G_CALLBACK(notebook_tab_close_button_style_set), NULL);
	}

	gtk_widget_show_all(ebox);

	document_update_tab_label(this);

	if (main_status.opening_session_files)
		cur_page = -1; /* last page */
	else if (file_prefs.tab_order_beside)
	{
		cur_page = gtk_notebook_get_current_page(GTK_NOTEBOOK(main_widgets.notebook));
		if (file_prefs.tab_order_ltr)
			cur_page++;
	}
	else if (file_prefs.tab_order_ltr)
		cur_page = -1; /* last page */
	else
		cur_page = 0;

	/* used by document_get_from_notebook_child() */
	g_object_set_data(G_OBJECT(vbox), "geany_document", this);
	tabnum = gtk_notebook_insert_page_menu(GTK_NOTEBOOK(main_widgets.notebook), vbox,
		ebox, NULL, cur_page);

	tab_count_changed();

	/* enable tab DnD */
	gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(main_widgets.notebook), vbox, TRUE);

	return tabnum;
}


static void
notebook_tab_close_clicked_cb(GtkButton *button, gpointer data)
{
	GeanyDocument *doc = (GeanyDocument *) data;

	document_close(doc);
}


/* Always use this instead of gtk_notebook_remove_page(). */
void notebook_remove_page(gint page_num)
{
	gint page = gtk_notebook_get_current_page(GTK_NOTEBOOK(main_widgets.notebook));

	if (page_num == page)
	{
		if (file_prefs.tab_order_ltr)
			page += 1;
		else if (page > 0) /* never go negative, it would select the last page */
			page -= 1;

		if (file_prefs.tab_close_switch_to_mru)
		{
			GeanyDocument *last_doc;

			last_doc = g_queue_peek_nth(mru_docs, 0);
			if (DOC_VALID(last_doc))
				page = document_get_notebook_page(last_doc);
		}

		gtk_notebook_set_current_page(GTK_NOTEBOOK(main_widgets.notebook), page);
	}

	/* now remove the page (so we don't temporarily switch to the previous page) */
	gtk_notebook_remove_page(GTK_NOTEBOOK(main_widgets.notebook), page_num);

	tab_count_changed();
}


static void
on_window_drag_data_received(GtkWidget *widget, GdkDragContext *drag_context,
		gint x, gint y, GtkSelectionData *data, guint target_type,
		guint event_time, gpointer user_data)
{
	gboolean success = FALSE;
	gint length = gtk_selection_data_get_length(data);

	if (length > 0 && gtk_selection_data_get_format(data) == 8)
	{
		document_open_file_list((const gchar *)gtk_selection_data_get_data(data), length);

		success = TRUE;
	}
	gtk_drag_finish(drag_context, success, FALSE, event_time);
}
