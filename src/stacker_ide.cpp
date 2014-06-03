#if defined(STACKER_IDE)

#define NOMINMAX

#include <cstdarg>
#include <algorithm>
#include <vector>
#include <Windows.h>
#include <Commctrl.h>
#include <commdlg.h>

#include <Scintilla.h>
#include <SciLexer.h>

#include "stacker.h"
#include "stacker_direct2d.h"
#include "stacker_ide_resource.h"

#include "url_cache.h"
#include "text_template.h"

#pragma warning(disable: 4505)

#define ensure(p) ((p) ? (void)0 : abort())

#if defined(_MSC_VER) && !defined(snprintf)
	#define snprintf _snprintf
#endif

namespace stkr {

using namespace urlcache;

const int IDM_SAMPLES_FIRST = 10000;
const char * const DEFAULT_DOCUMENT_TITLE = "Untitled Document";
const unsigned URL_BUFFER_SIZE = 2048;

enum GuiUnitTest {
	GUT_NONE,
	GUT_STRUCTURE_CHANGE
};

struct GuiState {
	BackEnd *back_end;
	HWND dialog_window;
	HWND control_pane_dialog;
	HWND navigation_bar_dialog;
	HMENU main_menu;
	HACCEL accelerators;
	HWND dump_control;
	HWND control_group_control;
	HWND test_files_combo;
	WNDPROC edit_proc;
	HANDLE fixed_font;
	char *source;
	unsigned source_length;
	UrlCache *url_cache;
	System *system;
	Document *document;
	View *view;
	unsigned paint_clock;
	char mb1[65535 * 20];
	char mb2[65535 * 20];
	std::vector<char> dump_buffer;
	std::vector<char> window_text_buffer;
	bool need_dump_update;
	int parse_code;
	bool show_dump_pane;
	bool show_navigation_bar;
	bool show_control_pane;
	bool scroll_bar_visible;
	float doc_scroll_x;
	float doc_scroll_y;
	RECT doc_box;
	RECT frame_rect;
	RECT hsplitter_box;
	RECT vsplitter_box;
	int hsplitter_pos;
	int vsplitter_pos;
	bool moving_hsplitter;
	bool moving_vsplitter;
	bool doc_mouse_capture;
	std::vector<const char *> sample_resource_names;
	HWND source_control;
	SciFnDirect scintilla_direct;
	sptr_t scintilla_instance;
	uint64_t signature;
	bool ignore_editor_changes;
	GuiUnitTest active_test;
	void *test_state;
};

static bool gui_message_loop(GuiState *state);
static void gui_end_test(GuiState *state);
static void gui_begin_test(GuiState *state, GuiUnitTest type);

static bool load_file(const char *path, void **buffer, unsigned *size)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return false;
	fseek(f, 0, SEEK_END);
	uint32_t s = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = new char[s + 1];
	fread(b, 1, s, f);
	fclose(f);
	b[s] = '\0';
	*buffer = b;
	*size = s;
	return true;
}

/* Resource types used for resources that can be loaded via IDE urls. */
const char * const DATA_RESOURCE_TYPES[] = { "STACKER", "PNG", "JPEG", };
const unsigned NUM_DATA_RESOURCE_TYPES = sizeof(DATA_RESOURCE_TYPES) / 
	sizeof(DATA_RESOURCE_TYPES[0]);

/* Attempts to load resource data into a heap buffer. */
static bool load_resource(const char *name, void **buffer, unsigned *size)
{
	HRSRC resource_handle = NULL;
	for (unsigned i = 0; i < NUM_DATA_RESOURCE_TYPES; ++i) {
		resource_handle = FindResource(NULL, name, DATA_RESOURCE_TYPES[i]);
		if (resource_handle != NULL)
			break;
	}
	if (resource_handle == NULL)
		return false;
	HGLOBAL loaded_resource = LoadResource(NULL, resource_handle);
	if (loaded_resource == NULL)
		return false;
	void *data = LockResource(loaded_resource);
	unsigned length = SizeofResource(NULL, resource_handle);
	char *b = new char[length];
	memcpy(b, data, length);
	*buffer = (void *)b;
	*size = length;
	return true;
}

/* Local fetch callback for the URL cache. */
static bool local_fetch_callback(void *, const ParsedUrl *url, 
	void **out_data, unsigned *out_size, MimeType *out_mime_type)
{
	/* Is this a local URL? */
	bool is_local = false;
	bool is_ide_url = false;
	if (url->scheme_length == 0 || match_scheme(url, "file")) {
		is_local = true;
	} else if (match_scheme(url, "stacker")) {
		is_ide_url = url->host_length == strlen("ide") && 
			!memcmp(url->url + url->host_start, "ide", url->host_length);
		is_local = is_ide_url;
	}
	if (!is_local)
		return false;

	/* A query only? */
	*out_mime_type = guess_mime_type(url->url + url->extension_starts[0],
		url->extension_lengths[0]);
	if (out_data == NULL)
		return true;

	/* Make a null terminated path string. */
	char path[FILENAME_MAX + 1];
	if (url->path_length + 1 > sizeof(path))
		return false;
	const char *start = url->url + url->path_start;
	unsigned length = url->path_length;
	if (length != 0 && *start == '/') {
		start++;
		length--;
	}
	memcpy(path, start, length);
	path[length] = '\0';

	/* Try to read the file into a heap buffer. */
	if (is_ide_url) {
		return load_resource(path, out_data, out_size);
	} else {
		return load_file(path, out_data, out_size);
	}
}

static void gui_panic(const char *message, ...) 
{
	char buf[4096];
	va_list args;
	va_start(args, message);
	int length = vsprintf(buf, message, args);
	va_end(args);
	buf[sizeof(buf) - 1] = '\0';

	DWORD last_error = GetLastError();
	strcpy(buf + length, " Last error: ");
	length = strlen(buf);
	FormatMessageA(
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, last_error, 0, buf + length,
		sizeof(buf) - length - 1, NULL);

	MessageBoxA(NULL, buf, "Panic!", MB_ICONINFORMATION | MB_OK);

	exit(1);
}

static LRESULT gui_scintilla_message(GuiState *state, unsigned message, 
	WPARAM wp = 0, LPARAM lp = 0)
{
	return state->scintilla_direct(state->scintilla_instance, message, wp, lp);
}

static void gui_dump_append(GuiState *state, const char *text, ...);

static const char *gui_formatv(GuiState *state, const char *fmt, va_list args, 
	bool convert_crlf = false)
{
	char *buf = state->mb1;
	unsigned length = vsnprintf(buf, sizeof(state->mb1), fmt, args);
	if (convert_crlf) {
		unsigned j = 0;
		for (unsigned i = 0; i < length; ++i) {
			if (buf[i] == '\n')
				state->mb2[j++] = '\r';
			state->mb2[j++] = buf[i];
		}
		length = j;
		buf = state->mb2;
	}
	buf[length] = '\0';
	return buf;
}

static const char *gui_format(GuiState *state, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	const char *result = gui_formatv(state, fmt, args, false);
	va_end(args);
	return result;
}

static void gui_dump_update(GuiState *state)
{
	SetWindowTextA(state->dump_control, &state->dump_buffer[0]);
	SendMessage(state->dump_control, EM_LINESCROLL, 0, 10000); 
	state->need_dump_update = false;
}

static void gui_dump_set(GuiState *state, const char *text, ...)
{
	if (text == NULL)
		text = "";
	va_list args;
	va_start(args, text);
	const char *message = gui_formatv(state, text, args, true);
	unsigned msglen = strlen(message);
	va_end(args);

	state->dump_buffer.resize(msglen + 1);
	memcpy(&state->dump_buffer[0], message, msglen + 1);
	state->need_dump_update = true;
}

static void gui_dump_appendv(GuiState *state, const char *text, va_list args)
{
	const char *message = gui_formatv(state, text, args, true);
	unsigned msglen = strlen(message);
	unsigned buffer_length = state->dump_buffer.size();
	state->dump_buffer.resize(buffer_length + msglen);
	memcpy(&state->dump_buffer[buffer_length - 1], message, msglen);
	state->need_dump_update = true;
	OutputDebugStringA(message);
}

static void gui_dump_append(GuiState *state, const char *text, ...)
{
	va_list args;
	va_start(args, text);
	gui_dump_appendv(state, text, args);
	va_end(args);
}

static void gui_info_set(GuiState *state, int id, const char *text, ...)
{
	if (text == NULL)
		text = "";
	va_list args;
	va_start(args, text);
	const char *message = gui_formatv(state, text, args, true);
	va_end(args);

	HWND info_handle = GetDlgItem(state->control_pane_dialog, id);
	SetWindowTextA(info_handle, message);
}

static const char *gui_get_text(GuiState *state, int id, 
	unsigned *out_length = NULL)
{
	HWND hwnd = GetDlgItem(state->control_pane_dialog, id);
	if (hwnd == NULL)
		hwnd = GetDlgItem(state->navigation_bar_dialog, id);
	if (hwnd == NULL)
		hwnd = GetDlgItem(state->dialog_window, id);
	if (hwnd == NULL)
		return NULL;
	unsigned length = GetWindowTextLengthA(hwnd);
	state->window_text_buffer.resize(1 + length);
	char *buffer = &state->window_text_buffer[0];
	GetWindowTextA(hwnd, buffer, length + 1);
	if (out_length != NULL)
		*out_length = length;
	return buffer;
}

static bool gui_read_source_editor(GuiState *state)
{
	unsigned length = gui_scintilla_message(state, SCI_GETTEXTLENGTH);
	if (state->source == NULL || state->source_length < length) {
		delete [] state->source;
		state->source = new char[length + 1];
	}
	gui_scintilla_message(state, SCI_GETTEXT, length + 1, (LPARAM)state->source);
	state->source_length = length;
	uint64_t signature = stkr::murmur3_64(state->source, length);
	bool changed = signature != state->signature;
	state->signature = signature;
	return changed;
}

static void gui_update_document(GuiState *state)
{
	static const unsigned MAX_ERROR_LENGTH = 512;

	char message[MAX_ERROR_LENGTH];
	gui_end_test(state);
	reset_document(state->document);
	int code = parse(
		state->system, 
		state->document, 
		get_root(state->document), 
		(const char *)state->source, 
		state->source_length, 
		message, MAX_ERROR_LENGTH);
	state->parse_code = code;
	if (code != STKR_OK) {
		gui_dump_append(state, "parse() returned code %d: %s\n", code, message);
	} else {
		gui_dump_append(state, "parse() returned STKR_OK.\n");
	}
	InvalidateRect(state->dialog_window, &state->frame_rect, FALSE);
}

static const char *gui_file_name(GuiState *state, const char *defval = NULL)
{
	char buf[URL_BUFFER_SIZE];
	ParsedUrl *url = stkr::get_url(state->document, buf, sizeof(buf));
	if (url == NULL)
		return defval;
	const char *file_name = path_file_name(url->url);
	return *file_name != '\0' ? file_name : defval;
}

static void gui_notify_document_url(GuiState *state, const char *url)
{
	/* Update the URL box. */
	HWND url_box = GetDlgItem(state->navigation_bar_dialog, IDC_URL);
	SetWindowTextA(url_box, url);

	/* Update the window title. */
	char window_title[URL_BUFFER_SIZE];
	snprintf(window_title, sizeof(window_title), "stkr::ide - %s",
		gui_file_name(state, DEFAULT_DOCUMENT_TITLE));
	window_title[sizeof(window_title) - 1] = '\0';
	SetWindowTextA(state->dialog_window, window_title);
}

static void gui_populate_source_editor(GuiState *state, 
	const char *data, unsigned length)
{
	state->ignore_editor_changes = true;
	gui_scintilla_message(state, SCI_CANCEL);
	gui_scintilla_message(state, SCI_SETUNDOCOLLECTION, 0);
	gui_scintilla_message(state, SCI_EMPTYUNDOBUFFER);
	if (data != NULL) {
		gui_scintilla_message(state, SCI_CLEARALL);
		gui_scintilla_message(state, SCI_APPENDTEXT, length, (LPARAM)data);
	} else {
		gui_scintilla_message(state, SCI_CLEARALL);
	}
	gui_scintilla_message(state, SCI_SETUNDOCOLLECTION, 1);
	gui_scintilla_message(state, SCI_SETSAVEPOINT);
	gui_scintilla_message(state, SCI_GOTOPOS, 0);
	if (gui_read_source_editor(state))
		gui_update_document(state);
	state->ignore_editor_changes = false;
}

static void gui_load_url(GuiState *state, const char *url)
{
	stkr::navigate(state->document, url, URLP_NORMAL);
}

static void gui_load_sample(GuiState *state, int sample_index)
{
	char url[URL_BUFFER_SIZE];
	const char *name = state->sample_resource_names[sample_index];
	snprintf(url, sizeof(url), "stacker://ide/%s", name);
	url[sizeof(url) - 1] = '\0';
	gui_load_url(state, url);
}

static void gui_save_source(GuiState *state, const char *path)
{
	gui_read_source_editor(state);
	FILE *f = fopen(path, "wb");
	if (f != NULL) {
		fwrite(state->source, 1, state->source_length, f);
		fclose(f);
	}
	stkr::set_url(state->document, path);
	gui_notify_document_url(state, path);
	gui_scintilla_message(state, SCI_SETSAVEPOINT);
}

static bool gui_is_checked(GuiState *state, int id)
{
	HWND check_handle = GetDlgItem(state->control_pane_dialog, id);
	int rc = SendMessage(check_handle, BM_GETCHECK, 0, 0);
	return rc == BST_CHECKED;
}

static bool gui_is_menu_checked(GuiState *state, int id)
{
	MENUITEMINFO info = { 0 };
	info.cbSize = sizeof(info);
	info.fMask = MIIM_STATE;
	GetMenuItemInfo(state->main_menu, id, FALSE, &info);
	return (info.fState & MFS_CHECKED) != 0;
}

static void gui_set_menu_item_state(GuiState *state, int id, bool checked, 
	bool enabled = true)
{
	MENUITEMINFO info = { 0 };
	info.cbSize = sizeof(info);
	info.fMask = MIIM_STATE;
	if (!GetMenuItemInfo(state->main_menu, id, FALSE, &info))
		gui_panic("GetMenuItemInfo() failed.");
	if (checked)
		info.fState |= MFS_CHECKED;
	else
		info.fState &= ~MFS_CHECKED;
	if (enabled)
		info.fState &= ~MFS_DISABLED;
	else
		info.fState |= MFS_DISABLED;
	if (!SetMenuItemInfo(state->main_menu, id, FALSE, &info))
		gui_panic("SetMenuItemInfo() failed.");
	DrawMenuBar(state->dialog_window);
}

static void gui_toggle_menu_checked(GuiState *state, int id)
{
	bool checked = gui_is_menu_checked(state, id);
	gui_set_menu_item_state(state, id, !checked, true);
}

static void gui_init_check_boxes(GuiState *state)
{
	gui_set_menu_item_state(state, IDM_CONSTRAIN_WIDTH, true, true);
}

static void gui_read_check_boxes(GuiState *state)
{
	set_view_flags(state->view, VFLAG_DEBUG_OUTER_BOXES, 
		gui_is_menu_checked(state, IDM_SHOW_OUTER_BOXES));
	set_view_flags(state->view, VFLAG_DEBUG_PADDING_BOXES, 
		gui_is_menu_checked(state, IDM_SHOW_PADDING_BOXES));
	set_view_flags(state->view, VFLAG_DEBUG_CONTENT_BOXES, 
		gui_is_menu_checked(state, IDM_SHOW_CONTENT_BOXES));
	set_view_flags(state->view, VFLAG_DEBUG_MOUSE_HIT, 
		gui_is_menu_checked(state, IDM_SHOW_MOUSE_HIT_SET));
	
	set_view_flags(state->view, VFLAG_CONSTRAIN_DOCUMENT_WIDTH, 
		gui_is_menu_checked(state, IDM_CONSTRAIN_WIDTH));
	set_view_flags(state->view, VFLAG_CONSTRAIN_DOCUMENT_HEIGHT, 
		gui_is_menu_checked(state, IDM_CONSTRAIN_HEIGHT));

	set_document_flags(state->document, DOCFLAG_DEBUG_LAYOUT,   
		gui_is_menu_checked(state, IDM_LAYOUT_DIAGNOSTICS));
	set_document_flags(state->document, DOCFLAG_DEBUG_FULL_LAYOUT,   
		gui_is_menu_checked(state, IDM_FORCE_FULL_LAYOUT));
	set_document_flags(state->document, DOCFLAG_DEBUG_PARAGRAPHS,   
		gui_is_menu_checked(state, IDM_PARAGRAPH_DIAGNOSTICS));
	set_document_flags(state->document, DOCFLAG_ENABLE_SELECTION,   
		gui_is_menu_checked(state, IDM_ENABLE_MOUSE_SELECTION));

	state->show_dump_pane = gui_is_menu_checked(state, IDM_SHOW_DUMP_PANE);
	state->show_navigation_bar = gui_is_menu_checked(state, IDM_SHOW_NAVIGATION_BAR);
	state->show_control_pane = gui_is_menu_checked(state, IDM_SHOW_CONTROL_PANE);
}

static bool gui_is_modified(GuiState *state)
{
	int rc = gui_scintilla_message(state, SCI_GETMODIFY);
	return rc != 0;
}

const char * const OPEN_SAVE_FILTER = 
	"Stacker Markup (*.stacker)\0*.stacker\0"
	"All Files (*.*)\0*.*\0";

static void gui_open_file(GuiState *state)
{
	char path[MAX_PATH];
	path[0] = '\0';

	OPENFILENAME ofn = { 0 };
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = state->dialog_window;
	ofn.lpstrFile = path;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrFilter = OPEN_SAVE_FILTER;
	ofn.nFilterIndex = 1;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = NULL;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

	if (GetOpenFileName(&ofn))
		gui_load_url(state, path);
}

static bool gui_url_to_file_path(GuiState *state, const ParsedUrl *url, 
	char *buffer, unsigned buffer_size)
{
	state;
	if (url == NULL || (!match_scheme(url, "file") && 
		url->scheme_length != 0) || url->path_length <= 1 || 
		url->path_length + 1u > buffer_size)
		return false;
	const char *start = url->url + url->path_start;
	unsigned length = url->path_length;
	if (length != 0 && *start == '/') {
		start++;
		length--;
	}
	snprintf(buffer, buffer_size, start, length);
	buffer[buffer_size - 1] = '\0';
	return true;
}

static bool gui_save_file(GuiState *state, bool save_as)
{
	/* Get the URL associated with the document. */
	char url_buffer[2048];
	ParsedUrl *url = stkr::get_url(state->document, 
		url_buffer, sizeof(url_buffer));

	/* If we have a URL, is it a disk path that we can save to? */
	const char *path = NULL;
	char path_buffer[FILENAME_MAX + 1];
	if (url != NULL && gui_url_to_file_path(state, url, 
		path_buffer, sizeof(path_buffer)))
		path = path_buffer;

	/* If we don't have a path to save to or this is "save as", show a file
	 * picker. */
	if (path == NULL || save_as) {
		OPENFILENAME ofn = { 0 };
		
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = state->dialog_window;
		ofn.lpstrFile = path_buffer;
		ofn.nMaxFile = sizeof(path_buffer);
		ofn.lpstrFilter = OPEN_SAVE_FILTER;
		ofn.nFilterIndex = 1;
		ofn.lpstrFileTitle = NULL;
		ofn.nMaxFileTitle = 0;
		ofn.lpstrInitialDir = NULL;
		ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_OVERWRITEPROMPT;

		path_buffer[0] = '\0';
		if (GetSaveFileName(&ofn))
			path = path_buffer;
	}
	if (path != NULL) {
		gui_save_source(state, path);
		return true;
	}
	return false;
}

static bool gui_save_prompt(GuiState *state)
{
	if (!gui_is_modified(state))
		return true;
	const char *message = gui_format(state, "Save changes to %s?",
		gui_file_name(state, DEFAULT_DOCUMENT_TITLE));
	int rc = MessageBox(state->dialog_window, message, "Confirmation",
		MB_YESNOCANCEL | MB_ICONQUESTION);
	if (rc == IDYES)
		return gui_save_file(state, false);
	return (rc == IDNO);
}

static void gui_new_file(GuiState *state)
{
	if (!gui_save_prompt(state))
		return;
	stkr::set_url(state->document, NULL);
	gui_notify_document_url(state, NULL);
	gui_populate_source_editor(state, NULL, 0);
}

static HMENU gui_get_samples_submenu(GuiState *state)
{
	HMENU file_menu = GetSubMenu(state->main_menu, 0);
	HMENU samples_menu = GetSubMenu(file_menu, 2);
	return samples_menu;
}

/* Callback used to populate the sample files menu. */
static BOOL CALLBACK gui_resource_name_callback(HMODULE hmodule, 
	LPCTSTR type, LPTSTR resource_name, LONG_PTR param)
{
	hmodule; type;

	GuiState *state = (GuiState *)param;
	unsigned length = strlen(resource_name);
	char *name = new char[length + 1];
	for (unsigned i = 0; i <= length ; ++i) 
		name[i] = (char)tolower(resource_name[i]);

	HMENU samples_menu = gui_get_samples_submenu(state);
	AppendMenu(samples_menu, MF_STRING, IDM_SAMPLES_FIRST + 
		state->sample_resource_names.size(), 
		path_file_name(name));
	state->sample_resource_names.push_back(name);
	return TRUE;
}

/* Populates the sample files submenu by enumerating resources. */
static void gui_populate_sample_menu(GuiState *state)
{
	HMENU samples_menu = gui_get_samples_submenu(state);
	RemoveMenu(samples_menu, IDM_SAMPLE_DOCUMENTS_PLACEHOLDER, MF_BYCOMMAND);
	EnumResourceNames(GetModuleHandle(NULL), "STACKER", 
		(ENUMRESNAMEPROC)&gui_resource_name_callback, 
		(LONG_PTR)state);
}

static void gui_navigate(GuiState *state)
{
	const char *url = gui_get_text(state, IDC_URL);
	if (!gui_save_prompt(state))
		return;
	int rc = stkr::navigate(state->document, url);
	gui_dump_append(state, "Attempting navigation to %s. stkr::navigate() "
		"returned code %d.\n", url, rc);
}

static void gui_quit(GuiState *state)
{
	PostMessage(state->dialog_window, WM_CLOSE, 0, 0);
}

static BOOL CALLBACK gui_control_group_dialog_proc(HWND hwnd, unsigned message, 
	WPARAM wp, LPARAM lp)
{
	GuiState *state = NULL;
	if (message == WM_INITDIALOG) {
		state = (GuiState *)lp;
		SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG)state);
		state->control_group_control = GetDlgItem(hwnd, IDC_CONTROL_GROUP);
		return FALSE;
	} else {
		state = (GuiState *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
	}

	if (message == WM_COMMAND) {
		unsigned id = LOWORD(wp);
		unsigned code = HIWORD(wp);
		if (code == BN_CLICKED) {
			if (id == IDB_CLEAR_DUMP) {
				gui_dump_set(state, NULL);
				return TRUE;
			} else if (id == IDB_MATCH_SELECTOR) {
				static const unsigned MAX_MATCHED_NODES = 1024;
				const Node *matched_nodes[MAX_MATCHED_NODES];
				const char *selector = gui_get_text(state, IDC_PARAM_1);
				int rc = match_nodes(state->document, NULL, selector, -1, 
					matched_nodes, MAX_MATCHED_NODES, -1);
				if (rc >= 0) {
					gui_dump_set(state, "Selector \"%s\" matched %d nodes:\n", selector, rc);
					for (unsigned i = 0; i < (unsigned)rc; ++i) {
						gui_dump_append(state, "%3u: %s\n", i, 
							get_node_debug_string(matched_nodes[i]));
					}
				} else {
					gui_dump_set(state, "Selector \"%s\" failed to parsed with code %d.\n", selector, rc);
				}
			}
			gui_read_check_boxes(state);
			gui_dump_update(state);
		}
		return TRUE;
	} else if (message == WM_SIZE) {
		RECT client_rect;
		GetClientRect(hwnd, &client_rect);
		MoveWindow(state->control_group_control, 
			client_rect.left,
			client_rect.top,
			client_rect.right - client_rect.left,
			client_rect.bottom - client_rect.top,
			TRUE);
		return TRUE;
	}
	return FALSE;
}

static BOOL CALLBACK gui_navigation_bar_dialog_proc(HWND hwnd, unsigned message, 
	WPARAM wp, LPARAM lp)
{
	GuiState *state = NULL;
	if (message == WM_INITDIALOG) {
		state = (GuiState *)lp;
		SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG)state);
		return FALSE;
	} else {
		state = (GuiState *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
	}

	if (message == WM_COMMAND) {
		unsigned id = LOWORD(wp);
		//unsigned code = HIWORD(wp);
		if (id == IDB_FETCH) {
			gui_navigate(state);
			return TRUE;
		}
		return TRUE;
	} else if (message == WM_SIZE) {
		static const int PADDING = 0;
		static const int BUTTON_GAP = 8;
		static const int FETCH_BUTTON_WIDTH = 60;

		RECT client_rect;
		GetClientRect(hwnd, &client_rect);
		int width = client_rect.right - client_rect.left;
		int height = client_rect.bottom - client_rect.top;
		int internal_width = width - 2 * PADDING;
		int internal_height = height - 2 * PADDING;
		int url_width = internal_width - FETCH_BUTTON_WIDTH - BUTTON_GAP;
		
		HWND url_combo = GetDlgItem(hwnd, IDC_URL);
		HWND fetch_button = GetDlgItem(hwnd, IDB_FETCH);
		
		MoveWindow(url_combo, 
			client_rect.left + PADDING,
			client_rect.top + PADDING,
			url_width,
			internal_height,
			TRUE);

		MoveWindow(fetch_button,
			client_rect.right - PADDING - FETCH_BUTTON_WIDTH,
			client_rect.top,
			FETCH_BUTTON_WIDTH,
			internal_height,
			TRUE);

		return TRUE;
	}
	return FALSE;
}

static void gui_update_indicators(GuiState *state)
{
	const Document *doc = state->document;

	gui_info_set(state, IDC_INFO1, "Paint clock: %u", state->paint_clock);
	gui_info_set(state, IDC_INFO2, "Layout clock: %u", get_layout_clock(doc));
	gui_info_set(state, IDC_INFO3, "Total nodes: %u", get_total_nodes(state->system));
	gui_info_set(state, IDC_INFO4, "Total boxes: %u", get_total_boxes(state->system));

	const Box *anchor_a = get_selection_start_anchor(doc);
	const Box *anchor_b = get_selection_end_anchor(doc);
	gui_info_set(state, IDC_SELECTION_START_ANCHOR, "Anchor A: %s", 
		get_box_debug_string(anchor_a, "N/A"));
	gui_info_set(state, IDC_SELECTION_END_ANCHOR, "Anchor B: %s", 
		get_box_debug_string(anchor_b, "N/A"));

	CaretAddress start = get_selection_start(doc);
	CaretAddress end = get_selection_end(doc);
	gui_info_set(state, IDC_SELECTION_START_NODE, "Caret A: %s/%d/%d", 
		get_node_debug_string(start.node, "N/A"), 
		start.ia.token, start.ia.offset);
	gui_info_set(state, IDC_SELECTION_END_NODE, "Caret B: %s/%d/%d", 
		get_node_debug_string(end.node, "N/A"), 
		end.ia.token, end.ia.offset);

	gui_info_set(state, IDC_SELECTION_INFO_1, "Flags: %s",
		((get_flags(doc) & DOCFLAG_SELECTING) != 0 ? "DOCFLAG_SELECTING" : ""));
}

static void gui_update_view_bounds(GuiState *state)
{
	float x = state->doc_scroll_x;
	float y = state->doc_scroll_y;
	float doc_box_width = float(state->doc_box.right - state->doc_box.left);
	float doc_box_height = float(state->doc_box.bottom - state->doc_box.top);
	set_view_bounds(state->view, x, x + doc_box_width, y, y + doc_box_height);
}

static void gui_set_scroll_pos(GuiState *state, float x, float y)
{
	/* Update the view bounds. */
	state->doc_scroll_x = x;
	state->doc_scroll_y = y;
	gui_update_view_bounds(state);

	/* Update the vertical scroll bar. */
	HWND scroll_bar_handle = GetDlgItem(state->dialog_window, IDC_DOC_VSCROLL);
	SCROLLINFO info;
	info.cbSize = sizeof(SCROLLINFO);
	info.fMask = SIF_POS;
	info.nPos = int(y + 0.5f);
	SendMessage(scroll_bar_handle, SBM_SETSCROLLINFO, TRUE, (LPARAM)&info);
}

static void gui_configure_scroll_bar(GuiState *state, HWND scroll_bar_handle,
	float doc_box_height, float doc_height)
{
	SCROLLINFO info;
	info.cbSize = sizeof(SCROLLINFO);
	info.fMask = SIF_PAGE | SIF_RANGE;
	info.nMin = 0;
	info.nMax = int(doc_height + 0.5f);
	info.nPage = int(doc_box_height + 0.5f);
	SendMessage(scroll_bar_handle, SBM_SETSCROLLINFO, TRUE, (LPARAM)&info);

	info.fMask = SIF_POS;
	SendMessage(scroll_bar_handle, SBM_GETSCROLLINFO, 0, (LPARAM)&info);
	gui_set_scroll_pos(state, state->doc_scroll_x, (float)info.nPos);
}

static const int GUI_VIEWER_PADDING = 8;
static const int GUI_VIEWER_DUMP_HEIGHT = 160;
static const int GUI_VIEWER_CONTROL_GROUP_HEIGHT = 100;
static const int GUI_VIEWER_NAV_BAR_HEIGHT = 24;
static const int GUI_VIEWER_SCROLL_BAR_WIDTH = 18;
static const int GUI_VIEWER_SCROLL_BAR_GAP = 8;

static bool gui_update_scroll_bar(GuiState *state)
{
	HWND scroll_bar_handle = GetDlgItem(state->dialog_window, IDC_DOC_VSCROLL);
	const RECT *doc_box = &state->doc_box;

	/* Is the document's root taller than the current view box?  */
	float doc_box_height = float(doc_box->bottom - doc_box->top);
	float doc_height = get_root_dimension(state->document, AXIS_V);
	bool scroll_bar_required = doc_height > doc_box_height;

	/* If the visibility state of the bar is changing, we need to recalculate
	 * the document box to add or remove the width of the bar. This in turn
	 * changes the document's width, which may change its computed height. Since
	 * the size of the scroll bar thumb is based on this computed height, we
	 * can't configure the scroll bar until layout is recalculated. */
	bool shown_or_hidden = scroll_bar_required != state->scroll_bar_visible;
	state->scroll_bar_visible = scroll_bar_required;
	if (shown_or_hidden)
		return true;

	/* The doc box is correct. Position the scroll bar and configure its
	 * thumb height. */
	MoveWindow(scroll_bar_handle,
		doc_box->right + GUI_VIEWER_SCROLL_BAR_GAP, 
		doc_box->top, 
		GUI_VIEWER_SCROLL_BAR_WIDTH, 
		doc_box->bottom - doc_box->top, 
		TRUE);
	ShowWindow(scroll_bar_handle, scroll_bar_required ? SW_SHOW : SW_HIDE);
	gui_configure_scroll_bar(state, scroll_bar_handle, 
		doc_box_height, doc_height);
	return false;
}

static void gui_update_viewer_layout(GuiState *state)
{
	RECT client_rect;
	RECT nav_box;
	RECT doc_box;
	RECT source_box;
	RECT dump_box;
	RECT control_box;

	GetClientRect(state->dialog_window, &client_rect);
	int width = client_rect.right - client_rect.left;
	int height = client_rect.bottom - client_rect.top;
	int hsplitter_pos = state->hsplitter_pos;
	if (hsplitter_pos < 0)
		hsplitter_pos = width / 2;
	hsplitter_pos = std::min(hsplitter_pos, width - GUI_VIEWER_PADDING);
	hsplitter_pos = std::max(hsplitter_pos, 2 * GUI_VIEWER_PADDING);
	int vsplitter_pos = state->vsplitter_pos;
	if (vsplitter_pos < 0)
		vsplitter_pos = height - GUI_VIEWER_CONTROL_GROUP_HEIGHT - 
			GUI_VIEWER_DUMP_HEIGHT - 3 * GUI_VIEWER_PADDING;
	vsplitter_pos = std::min(vsplitter_pos, height - 
		GUI_VIEWER_CONTROL_GROUP_HEIGHT - 3 * GUI_VIEWER_PADDING);
	vsplitter_pos = std::max(vsplitter_pos, 3 * GUI_VIEWER_PADDING);
	unsigned source_width = width - 2 * GUI_VIEWER_PADDING - hsplitter_pos;

	/* Navigation bar. */
	nav_box.top = client_rect.top + GUI_VIEWER_PADDING;
	nav_box.bottom = nav_box.top;
	nav_box.left = client_rect.left + GUI_VIEWER_PADDING;
	nav_box.right = client_rect.right - GUI_VIEWER_PADDING;
	if (state->show_navigation_bar)
		nav_box.bottom += GUI_VIEWER_NAV_BAR_HEIGHT;
	else
		nav_box.bottom -= GUI_VIEWER_PADDING;

	/* Control pane. */
	control_box.left = client_rect.left + GUI_VIEWER_PADDING;
	control_box.top = client_rect.bottom - GUI_VIEWER_PADDING;
	control_box.right = client_rect.right - GUI_VIEWER_PADDING;
	control_box.bottom = client_rect.bottom - GUI_VIEWER_PADDING;
	if (state->show_control_pane)
		control_box.top -= GUI_VIEWER_CONTROL_GROUP_HEIGHT;

	/* Dump window. */
	dump_box.bottom = control_box.top - GUI_VIEWER_PADDING;
	dump_box.top = dump_box.bottom;
	dump_box.left = client_rect.left + GUI_VIEWER_PADDING;
	dump_box.right = client_rect.right - GUI_VIEWER_PADDING;
	if (state->show_dump_pane)
		dump_box.top = vsplitter_pos + GUI_VIEWER_PADDING;

	/* Source editor. */
	source_box.left = client_rect.right - GUI_VIEWER_PADDING - source_width;
	source_box.top = nav_box.bottom + GUI_VIEWER_PADDING;
	source_box.right = client_rect.right - GUI_VIEWER_PADDING;
	source_box.bottom = dump_box.top - GUI_VIEWER_PADDING;

	/* Build a document box, accounting for the width of the scroll bar if it
	 * is visible. If the document layout using this box changes the height in
	 * such a way that the scroll bar becomes required or is no longer required,
	 * repeat the process, recalculating the document layout with the widened
	 * or narrowed document box. */
	int tries = 2;
	do {
		doc_box.left = client_rect.left + GUI_VIEWER_PADDING;
		doc_box.top = nav_box.bottom + GUI_VIEWER_PADDING;
		doc_box.right = source_box.left - GUI_VIEWER_PADDING;
		doc_box.bottom = dump_box.top - GUI_VIEWER_PADDING;
		if (state->scroll_bar_visible)
			doc_box.right -= GUI_VIEWER_SCROLL_BAR_WIDTH + GUI_VIEWER_SCROLL_BAR_GAP;
		state->doc_box = doc_box;

		gui_update_view_bounds(state);
		update_document(state->document);
		update_view(state->view);
	} while (gui_update_scroll_bar(state) && --tries != 0);

	/* Splitter hit testing boxes. */
	state->hsplitter_box.left = doc_box.right;
	state->hsplitter_box.right = source_box.left;
	state->hsplitter_box.top = doc_box.top;
	state->hsplitter_box.bottom = doc_box.bottom;
	state->vsplitter_box.top = doc_box.bottom;
	state->vsplitter_box.bottom = dump_box.top;
	state->vsplitter_box.left = doc_box.left;
	state->vsplitter_box.right = source_box.right;

	/* Slightly enlarged version of the document area used to draw the document
	 * border. */
	state->frame_rect = state->doc_box;
	state->frame_rect.right += 2;
	state->frame_rect.bottom += 2;

	/* Position the child windows. */
	MoveWindow(state->source_control,  
		source_box.left,
		source_box.top,
		source_box.right - source_box.left,
		source_box.bottom - source_box.top,
		TRUE);

	ShowWindow(state->navigation_bar_dialog, 
		state->show_navigation_bar ? SW_SHOW : SW_HIDE);
	if (state->show_navigation_bar) {
		MoveWindow(state->navigation_bar_dialog,
			nav_box.left,
			nav_box.top,
			nav_box.right - nav_box.left,
			nav_box.bottom - nav_box.top,
			TRUE);
	}

	ShowWindow(state->dump_control, 
		state->show_dump_pane ? SW_SHOW : SW_HIDE);
	if (state->show_dump_pane) {
		MoveWindow(state->dump_control, 
			dump_box.left,
			dump_box.top,
			dump_box.right - dump_box.left,
			dump_box.bottom - dump_box.top,
			TRUE);
	}

	ShowWindow(state->control_pane_dialog, 
		state->show_control_pane ? SW_SHOW : SW_HIDE);
	if (state->show_control_pane) {
		MoveWindow(state->control_pane_dialog, 
			control_box.left,
			control_box.top,
			control_box.right - control_box.left,
			control_box.bottom - control_box.top,
			TRUE);
	}

	InvalidateRect(state->dialog_window, NULL, TRUE);
}

static bool gui_update_cursor(GuiState *state, CursorType ct)
{
	state;
	LPCSTR system_cursor = NULL;
	switch (ct) {
		case CT_DEFAULT:
			system_cursor = IDC_ARROW;
			break;
		case CT_HAND:
			system_cursor = IDC_HAND;
			break;
		case CT_CARET:
			system_cursor = IDC_IBEAM;
			break;
		case CT_CROSSHAIR:
			system_cursor = IDC_CROSS;
			break;
		case CT_MOVE:
			system_cursor = IDC_SIZEALL;
			break;
		case CT_SIZE_NS:
			system_cursor = IDC_SIZENS;
			break;
		case CT_SIZE_EW:
			system_cursor = IDC_SIZEWE;
			break;
		case CT_WAIT:
			system_cursor = IDC_WAIT;
			break;
	}
	if (system_cursor != NULL) {
		HCURSOR cursor_handle = LoadCursor(NULL, system_cursor);
		if (cursor_handle != NULL)
			SetCursor(cursor_handle);
		return true;
	}
	return false;
}

static bool gui_check_paint_clock(GuiState *state)
{
	if (state->paint_clock != get_paint_clock(state->view)) {
		InvalidateRect(state->dialog_window, &state->frame_rect, TRUE);
		return true;
	}
	return false;
}

static void gui_handle_navigation_message(GuiState *state, 
	const Message *message)
{
	char buf[2048];
	ParsedUrl *url = get_url(state->document, buf, sizeof(buf));

	/* Update the navigation bar with the document's current URL. */
	gui_notify_document_url(state, url != NULL ? url->url : NULL);

	/* Populate the source buffer if the source has changed. */
	if (message->navigation.new_state == DOCNAV_SUCCESS ||
	    message->navigation.new_state == DOCNAV_PARSE_ERROR) {
		unsigned source_length = 0;
		const char *source = get_source(state->document, &source_length);
		gui_populate_source_editor(state, source, source_length);
	}

	/* Print a helpful message. */
	const char *url_desc = url != NULL ? url->url : "NULL";
	gui_dump_append(state, "MSG_NAVIGATE: %u => %u. "
		"URL is \"%s\".\n", 
		message->navigation.old_state,
		message->navigation.new_state,
		url_desc);

}

static void gui_handle_document_messages(GuiState *state)
{
	const Message *message;
	do {
		message = dequeue_message(state->document);
		if (message == NULL)
			break;
		if (message->type == MSG_CURSOR_CHANGED) {
			gui_update_cursor(state, message->cursor.cursor);
		} else if (message->type == MSG_NAVIGATE) {
			gui_handle_navigation_message(state, message);
		} else if (message->type == MSG_NODE_ACTIVATED) {
			gui_dump_append(state, "MSG_NODE_ACTIVATED: %s\n", 
				get_node_debug_string(message->activation.node));
		} else if (message->type == MSG_NODE_EXPANDED) {
			gui_dump_append(state, "MSG_NODE_EXPANDED: %s: "
				"left: %u, right: %u, up: %u, down: %u\n", 
				get_node_debug_string(message->expansion.node),
				(message->flags & EMF_EXPANDED_LEFT)  != 0,
				(message->flags & EMF_EXPANDED_RIGHT) != 0,
				(message->flags & EMF_EXPANDED_UP)    != 0,
				(message->flags & EMF_EXPANDED_DOWN)  != 0);
		}
	} while (message != NULL);
}

struct StructureChangeTestState {
	Rule *rule_even;
	Rule *rule_odd;
	unsigned step;
};

static void gui_update_structure_change_test(GuiState *state, 
	StructureChangeTestState *sts)
{
	Document *document = state->document;
	if (sts == NULL) {
		gui_new_file(state);

		sts = new StructureChangeTestState();
		state->test_state = (char *)sts;
		
		sts->step = 0;
		set_node_flags(document, get_root(document), 
			NFLAG_NOTIFY_EXPANSION, true);

		AttributeAssignment rule_attributes[2];
		rule_attributes[0] = make_assignment(TOKEN_LAYOUT, TOKEN_INLINE_CONTAINER, VSEM_TOKEN);
		rule_attributes[1] = make_assignment(TOKEN_COLOR, 0xFFFF0000, VSEM_COLOR);

		add_rule(
			&sts->rule_even, 
			state->system, 
			document, 
			".even", -1,
			rule_attributes,
			2,
			RFLAG_ENABLED,
			RULE_PRIORITY_OVERRIDE);

		variant_set_integer(&rule_attributes[1].value, 0xFF00FF00, VSEM_COLOR);
		add_rule(
			&sts->rule_odd, 
			state->system, 
			document, 
			".odd", -1,
			rule_attributes, 
			2,
			RFLAG_ENABLED,
			RULE_PRIORITY_OVERRIDE);
	}
}

bool gui_structure_change_test_handle_message(GuiState *state, 
	StructureChangeTestState *sts, int message, WPARAM wp, LPARAM lp)
{
	lp;

	static const unsigned APPEND_COUNT = 50000;

	Document *document = state->document;

	if (message == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
		bool above = (wp == VK_UP);

		gui_dump_append(state, 
			"Structure change test: inserting nodes %s.\n",
			(above ? "above" : "below"));
		for (unsigned i = 0; i < APPEND_COUNT; ++i) {
			
			char text[512];
			sprintf(text, "Message %u", sts->step);

			AttributeAssignment attributes[2];
			attributes[0] =  make_assignment(TOKEN_CLASS, 
				((sts->step % 2) == 0) ? "even" : "odd",
				VSEM_LIST);
			attributes[1] = make_assignment(TOKEN_LAYOUT, TOKEN_NONE, 
				VSEM_TOKEN);

			Node *container = NULL;
			int rc = create_node(
				&container, document, 
				LNODE_PARAGRAPH, 
				TOKEN_PARAGRAPH, 
				attributes, 
				2, 
				text, strlen(text));
			if (rc == STKR_OK) {
				if (above)
					prepend_child(document, get_root(document), container);
				else
					append_child(document, get_root(document), container);
			}

			sts->step++;
		}

		return true;
	}

	if (message == WM_CHAR && (wp == 'o' || wp == 'e')) {
		bool odd = (wp == 'o');
		Rule *rule = odd ? sts->rule_odd : sts->rule_even;

		unsigned flags = get_rule_flags(rule);
		bool enabled = (flags & RFLAG_ENABLED) != 0;
		gui_dump_append(state, "Setting RFLAG_ENABLED  for %s rule to %d.\n",
			(odd ? "odd" : "even"), !enabled);
		set_rule_flags(rule, RFLAG_ENABLED, !enabled);
		return true;
	}

	return false;
}

static void gui_update_test(GuiState *state)
{
	switch (state->active_test) {
		case GUT_NONE:
			break;
		case GUT_STRUCTURE_CHANGE:
			gui_update_structure_change_test(state, 
				(StructureChangeTestState *)state->test_state);
			break;
	}
}

static bool gui_test_handle_message(GuiState *state, int message, 
	WPARAM wp, LPARAM lp)
{
	switch (state->active_test) {
		case GUT_NONE:
			break;
		case GUT_STRUCTURE_CHANGE:
			return gui_structure_change_test_handle_message(state, 
				(StructureChangeTestState *)state->test_state,
				message, wp, lp);
			break;
	}
	return false;
}

static void gui_end_test(GuiState *state)
{
	if (state->active_test == GUT_NONE)
		return;
	if (state->test_state != NULL) {
		delete [] state->test_state;
		state->test_state = NULL;
	}
	gui_dump_append(state, "Terminated unit test %u.\n", state->active_test);
	state->active_test = GUT_NONE;
}

static void gui_begin_test(GuiState *state, GuiUnitTest type)
{
	gui_end_test(state);
	state->active_test = type;
	gui_dump_append(state, "Beginning unit test %u.\n", type);
	gui_update_test(state);
}

static const uint32_t LEXER_COLOR_DEFAULT = 0xFF000000;

static const struct {
	int style;
	uint32_t foreground_color;
	uint32_t background_color;
	unsigned font_size;
	const char *font_face;
	bool bold;
	bool italic;
} LEXER_STYLE_OVERRIDES[] = {
	{ SCE_H_VALUE,            RGB(100, 169, 189), LEXER_COLOR_DEFAULT, 0, NULL, false, false },
	{ SCE_H_DEFAULT,          0xdcdccc,           LEXER_COLOR_DEFAULT, 0, NULL, false, false },
	{ SCE_H_OTHER,            RGB(128, 128, 128), LEXER_COLOR_DEFAULT, 0, NULL, false, false },
	{ SCE_H_COMMENT,          0x7f9f7f,           LEXER_COLOR_DEFAULT, 0, NULL, false, false },
	{ SCE_H_DOUBLESTRING,     RGB(149, 228, 84),  LEXER_COLOR_DEFAULT, 0, NULL, false, false },
	{ SCE_H_SINGLESTRING,     RGB(149, 228, 84),  LEXER_COLOR_DEFAULT, 0, NULL, false, false },
	{ SCE_H_NUMBER,           0xCC66FF,           LEXER_COLOR_DEFAULT, 0, NULL, false, false },
	{ SCE_H_TAGUNKNOWN,       0xffff00,           0xff0000,            0, NULL, false, false },
	{ SCE_H_TAG,              RGB(202, 230, 130), LEXER_COLOR_DEFAULT, 0, NULL, true,  false },
	{ SCE_H_TAGEND,           RGB(202, 230, 130), LEXER_COLOR_DEFAULT, 0, NULL, true,  false },
	{ SCE_H_ATTRIBUTE,        0xcfcfcf,           LEXER_COLOR_DEFAULT, 0, NULL, false,  false },
	{ SCE_H_ATTRIBUTEUNKNOWN, 0xcfcfcf,           LEXER_COLOR_DEFAULT, 0, NULL, false,  false },
	{ STYLE_LINENUMBER,       0x9fafaf,           0x262626,            0, NULL, false, false },
	{ STYLE_BRACELIGHT,       0xb2b2a0,           LEXER_COLOR_DEFAULT, 0, NULL, true,  false },
	{ STYLE_BRACEBAD,         0xeeb2a0,           LEXER_COLOR_DEFAULT, 0, NULL, false, false },
	{ STYLE_INDENTGUIDE,      RGB(128, 128, 128), LEXER_COLOR_DEFAULT, 0, NULL, false, false }
};
static const unsigned NUM_LEXER_STYLE_OVERRIDES = 
	sizeof(LEXER_STYLE_OVERRIDES) / sizeof(LEXER_STYLE_OVERRIDES[0]);

static void gui_set_lexer_style(
	GuiState *state, 
	int style,
	uint32_t foreground_color, 
	uint32_t background_color = RGB(0, 4, 0),
	unsigned font_size = 0,
	const char *font_face = NULL,
	bool bold = false, 
	bool italic = false)
{
	if (foreground_color != LEXER_COLOR_DEFAULT)
		gui_scintilla_message(state, SCI_STYLESETFORE, style, foreground_color);
	if (background_color != LEXER_COLOR_DEFAULT)
		gui_scintilla_message(state, SCI_STYLESETBACK, style, background_color);
	if (font_size != 0)
		gui_scintilla_message(state, SCI_STYLESETSIZE, style, font_size);
	if (font_face != 0)
		gui_scintilla_message(state, SCI_STYLESETFONT, style, (LPARAM)font_face);
	gui_scintilla_message(state, SCI_STYLESETBOLD, style, bold);
	gui_scintilla_message(state, SCI_STYLESETITALIC, style, italic);
}

static void gui_configure_scintilla(GuiState *state)
{
	gui_scintilla_message(state, SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
	gui_scintilla_message(state, SCI_SETMARGINWIDTHN, 0, 40);
	gui_scintilla_message(state, SCI_SETTABWIDTH, 4);
	gui_scintilla_message(state, SCI_SETUSETABS, true);
	gui_scintilla_message(state, SCI_SETTABINDENTS, true);
	gui_scintilla_message(state, SCI_SETINDENT, 4);
	gui_scintilla_message(state, SCI_SETINDENTATIONGUIDES, SC_IV_LOOKBOTH);
	gui_scintilla_message(state, SCI_SETCARETFORE, 0x8faf9f);
	gui_scintilla_message(state, SCI_SETSELBACK, true, 0xA0A0C0);
	gui_scintilla_message(state, SCI_SETSELFORE, true, 0xFFFFFF);

	gui_scintilla_message(state, SCI_SETWRAPMODE, SC_WRAP_NONE);
	gui_scintilla_message(state, SCI_SETWRAPVISUALFLAGS, SC_WRAPVISUALFLAG_END | SC_WRAPVISUALFLAG_MARGIN);
}

static void gui_configure_scintilla_lexer(GuiState *state)
{
	/* Make a space-delimited string containing all Stacker keywords to pass
	 * to Scintilla. */
	unsigned keywords_length = 0;
	for (unsigned i = stkr::TOKEN_KEYWORD_FIRST; 
		i != stkr::TOKEN_KEYWORD_LAST; ++i)
		keywords_length += strlen(stkr::TOKEN_STRINGS[i]) + 1;
	char *keywords = new char[keywords_length], *pos = keywords;
	for (unsigned i = stkr::TOKEN_KEYWORD_FIRST; 
		i != stkr::TOKEN_KEYWORD_LAST; ++i) {
		if (i != stkr::TOKEN_KEYWORD_FIRST)
			*pos++ = ' ';
		strcpy(pos, stkr::TOKEN_STRINGS[i]);
		pos += strlen(stkr::TOKEN_STRINGS[i]);
	}
	*pos = '\0';

	/* Configure the lexer. */
	gui_scintilla_message(state, SCI_SETLEXER, SCLEX_HTML);
	gui_scintilla_message(state, SCI_SETSTYLEBITS, 7);
	gui_scintilla_message(state, SCI_SETKEYWORDS, 0, (LPARAM)keywords);
	delete [] keywords;

	/* Set the default style and propagate it to all styles. */
	gui_set_lexer_style(state, STYLE_DEFAULT, 0xdcdccc, 0x3f3f3f, 11, "Consolas");
	gui_scintilla_message(state, SCI_STYLECLEARALL);
	
	/* Non-content styles. */
	for (unsigned i = 0; i < NUM_LEXER_STYLE_OVERRIDES; ++i) {
		gui_set_lexer_style(state, 
			LEXER_STYLE_OVERRIDES[i].style,
			LEXER_STYLE_OVERRIDES[i].foreground_color,
			LEXER_STYLE_OVERRIDES[i].background_color,
			LEXER_STYLE_OVERRIDES[i].font_size,
			LEXER_STYLE_OVERRIDES[i].font_face,
			LEXER_STYLE_OVERRIDES[i].bold,
			LEXER_STYLE_OVERRIDES[i].italic);
	}
}

static HWND gui_create_scintilla_editor(GuiState *state, HWND parent)
{
	HWND hwnd = CreateWindow(
		"Scintilla",
		"Source Editor",
		WS_CHILD | WS_TABSTOP | WS_CLIPCHILDREN,
		0, 0, 0, 0, 
		parent, (HMENU)IDC_SOURCE_TEXT,
		GetModuleHandle(NULL),
		0);
	if (hwnd == NULL)
		gui_panic("Failed to create Scintilla editor.");
	state->scintilla_direct = (SciFnDirect)SendMessage(hwnd, SCI_GETDIRECTFUNCTION, 0, 0);
	state->scintilla_instance = (sptr_t)SendMessage(hwnd, SCI_GETDIRECTPOINTER, 0, 0);

	gui_configure_scintilla(state);
	gui_configure_scintilla_lexer(state);

	return hwnd;
}

static LRESULT CALLBACK gui_edit_subclass(HWND hwnd, unsigned message, 
	WPARAM wp, LPARAM lp)
{
	GuiState *state = (GuiState *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
	LRESULT lr = CallWindowProc(state->edit_proc, hwnd, message, wp, lp);
	if (message == WM_GETDLGCODE)
		lr = 0;
	return lr;
}

static BOOL CALLBACK gui_dialog_proc(HWND hwnd, unsigned message, WPARAM wp, LPARAM lp)
{
	GuiState *state = NULL;

	if (message == WM_INITDIALOG) {
		state = (GuiState *)lp;
		SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG)state);
		HINSTANCE module = (HINSTANCE)GetModuleHandle(NULL);

		/* Configure child controls. */
		state->dialog_window = hwnd;
		state->source_control = gui_create_scintilla_editor(state, hwnd);
		state->dump_control = GetDlgItem(hwnd, IDC_DUMP);
		SendMessage(state->dump_control, WM_SETFONT, (WPARAM)state->fixed_font, 0);

		/* Subclass the dump control so that it doesn't steal our key presses. */
		SetWindowLongPtr(state->dump_control, GWLP_USERDATA, (LONG_PTR)state);
		state->edit_proc = (WNDPROC)SetWindowLongPtr(state->dump_control, 
			GWLP_WNDPROC, (LONG_PTR)gui_edit_subclass);

		/* Create the control pane dialog. */
		state->control_pane_dialog = CreateDialogParam(module,
			MAKEINTRESOURCE(IDD_CONTROL_GROUP), hwnd, 
			&gui_control_group_dialog_proc, lp);
		if (state->control_group_control == NULL)
			gui_panic("Failed to create control group dialog.");

		/* Create the navigation bar dialog. */
		state->navigation_bar_dialog = CreateDialogParam(module,
			MAKEINTRESOURCE(IDD_NAVIGATION_BAR), hwnd,
			 &gui_navigation_bar_dialog_proc, lp);
		if (state->control_group_control == NULL)
			gui_panic("Failed to create navigation bar dialog.");

		/* Create the main menu. */
		state->main_menu = LoadMenu(module, MAKEINTRESOURCE(IDM_MAIN));
		if (state->main_menu == NULL)
			gui_panic("Failed to load main menu.");
		gui_populate_sample_menu(state);
		SetMenu(hwnd, state->main_menu);

		ShowWindow(state->source_control, SW_SHOW);
		ShowWindow(state->control_group_control, SW_SHOW);
		ShowWindow(state->navigation_bar_dialog, SW_SHOW);
		SetFocus(state->source_control);
		return FALSE;
	} else {
		state = (GuiState *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
	}

	if (state != NULL && gui_test_handle_message(state, message, wp, lp))
		return TRUE;

	if (message == WM_CLOSE) {
		if (gui_save_prompt(state)) {
			DestroyWindow(hwnd);
			PostQuitMessage(0);
		}
		return TRUE;
	} else if (message == WM_COMMAND) {
		unsigned id = LOWORD(wp);
		unsigned code = HIWORD(wp);
		if (id == IDOK) {
			gui_navigate(state);
			return TRUE;
		} else if (code <= 1) {
			if (id == IDM_SHOW_CONTROL_PANE           || 
				id == IDM_SHOW_DUMP_PANE              || 
				id == IDM_SHOW_NAVIGATION_BAR         ||
				id == IDM_SHOW_CONTENT_BOXES          ||
				id == IDM_SHOW_PADDING_BOXES          ||
				id == IDM_SHOW_OUTER_BOXES            ||
				id == IDM_SHOW_MOUSE_HIT_SET          ||
				id == IDM_CONSTRAIN_WIDTH             ||
				id == IDM_CONSTRAIN_HEIGHT            ||
				id == IDM_LAYOUT_DIAGNOSTICS          ||
				id == IDM_FORCE_FULL_LAYOUT           ||
				id == IDM_PARAGRAPH_DIAGNOSTICS       ||
				id == IDM_ENABLE_MOUSE_SELECTION) {
				gui_toggle_menu_checked(state, id);
				gui_read_check_boxes(state);
				if (id == IDM_SHOW_CONTROL_PANE || 
				    id == IDM_SHOW_DUMP_PANE ||
				    id == IDM_SHOW_NAVIGATION_BAR) {
					gui_update_viewer_layout(state);
				}
				return TRUE;
			} else if (id == IDM_COPY) {
				SetFocus(state->dialog_window);
				view_handle_keyboard_event(state->view, MSG_KEY_DOWN, 'c', KMF_CTRL);
				return TRUE;
			} else if (id == IDM_CLEAR_DUMP) {
				gui_dump_set(state, NULL);
				return TRUE;
			} else if (id == IDM_UPDATE_LAYOUT) {
				gui_dump_set(state, "=== LAYOUT DIAGNOSTICS ===\n\n");
				set_document_flags(state->document, DOCFLAG_DEBUG_LAYOUT, true);
				update_document(state->document);
				set_document_flags(state->document, DOCFLAG_DEBUG_LAYOUT,   
					gui_is_checked(state, IDC_LAYOUT_DIAGNOSTICS_CHECK));
				return TRUE;
			} else if (id == IDM_DUMP_NODES) {
				gui_dump_set(state, "NODE TREE\n\n");
				dump_node(state->document, get_root(state->document));
				return TRUE;
			} else if (id == IDM_DUMP_BOXES) {
				gui_dump_set(state, "BOX TREE\n\n");
				dump_boxes(state->document, get_box(get_root(state->document)));
				return TRUE;
			} else if (id == IDM_DUMP_BOX_QUADTREE) {
				gui_dump_set(state, "BOX QUADTREE\n\n");
				dump_grid(state->document);
				return TRUE;
			} else if (id == IDM_DUMP_RULE_TABLES) {
				gui_dump_set(state, "DOCUMENT RULE TABLE\n\n");
				dump_rule_table(state->document, false);
				gui_dump_append(state, "\n\nGLOBAL RULE TABLE\n\n");
				dump_rule_table(state->document, true);
				return TRUE;
			} else if (id == IDM_DUMP_INLINE_CONTEXTS) {
				gui_dump_set(state, "INLINE CONTEXT BUFFERS\n\n");
				dump_all_inline_contexts(state->document, get_root(state->document));
				return TRUE;
			} else if (id == IDM_TERMINATE_TEST) {
				gui_end_test(state);
				return TRUE;
			} else if (id == IDM_QUADTREE_UNIT_TEST) {
				gui_dump_set(state, "Running grid intersection test.\n\n");
				unit_test_box_grid(state->document);
				gui_dump_append(state, "Grid intersection test OK.\n\n");
				return TRUE;
			} else if (id == IDM_STRUCTURE_CHANGE_NOTIFICATION_TEST) {
				gui_begin_test(state, GUT_STRUCTURE_CHANGE);
				return TRUE;
			} else if (id == IDM_NEW_FILE) {
				gui_new_file(state);
				return TRUE;
			} else if (id == IDM_OPEN_FILE) {
				gui_open_file(state);
				return TRUE;
			} else if (unsigned(id - IDM_SAMPLES_FIRST) < 
				state->sample_resource_names.size()) {
				gui_load_sample(state, id - IDM_SAMPLES_FIRST);
			} else if (id == IDM_SAVE_FILE) {
				gui_save_file(state, false);
				return TRUE;
			} else if (id == IDM_SAVE_FILE_AS) {
				gui_save_file(state, true);
				return TRUE;
			} else if (id == IDM_QUIT) {
				gui_quit(state);
				return TRUE;
			}
		}
		return TRUE;
	} else if (message == WM_NOTIFY) {
		if (wp == IDC_SOURCE_TEXT) {
			const SCNotification *notification = (const SCNotification *)lp;
			unsigned code = notification->nmhdr.code;
			unsigned type = notification->modificationType;
			if (code == SCN_MODIFIED && !state->ignore_editor_changes && 
				(type & SC_PERFORMED_USER) != 0 &&
				((type & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) != 0)) {
				if (gui_read_source_editor(state)) {
					gui_dump_set(state, NULL);
					gui_update_document(state);
				}
				return TRUE;
			}
		} 
	} else if (message == WM_PAINT) {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);

		int doc_width = state->doc_box.right - state->doc_box.left;
		int doc_height = state->doc_box.bottom - state->doc_box.top;
		int frame_width = doc_width + 2;
		int frame_height = doc_height + 2;
		RECT draw_rect = { 1, 1, 1 + doc_width, 1 + doc_height };
		RECT frame_rect = { 0, 0, frame_width, frame_height };

		HBITMAP buffer_bitmap = CreateCompatibleBitmap(hdc, frame_width, frame_height);
		HDC memory_dc = CreateCompatibleDC(hdc);
		SelectObject(memory_dc, buffer_bitmap);

		/* Outline the view box. */
		HPEN outline_pen = CreatePen(PS_DASH, 1, RGB(128, 128, 128));
		HBRUSH fill_brush = CreateHatchBrush(HS_DIAGCROSS, RGB(224, 224, 224));
		HANDLE old_pen = SelectObject(memory_dc, outline_pen);
		HANDLE old_brush = SelectObject(memory_dc, fill_brush);
		Rectangle(memory_dc, 
			frame_rect.left, 
			frame_rect.top,
			frame_rect.right,
			frame_rect.bottom);
		if (old_brush != NULL)
			SelectObject(memory_dc, old_brush);
		if (old_pen != NULL)
			SelectObject(memory_dc, old_pen);
		DeleteObject(outline_pen);
		DeleteObject(fill_brush);

		/* Draw the view. */
		update_document(state->document);
		update_view(state->view);
		state->paint_clock = get_paint_clock(state->view);
		if (gui_update_scroll_bar(state))
			gui_update_viewer_layout(state);
		d2d_draw_view(state->back_end, state->view, state->dialog_window, 
			memory_dc, &draw_rect);

		BitBlt(hdc, 
			state->frame_rect.left, 
			state->frame_rect.top, 
			frame_width, frame_height,
			memory_dc, 0, 0, SRCCOPY);

		DeleteObject(buffer_bitmap); 
		DeleteDC(memory_dc);

		EndPaint(hwnd, &ps);

		gui_update_indicators(state);

		return TRUE;
	} else if (message == WM_ERASEBKGND) {
		HDC hdc = (HDC)wp;
		RECT client;
		GetClientRect(hwnd, &client);
		HBRUSH background_brush = GetSysColorBrush(COLOR_3DFACE);
		HRGN region = CreateRectRgn(client.left, client.top, client.right, client.bottom);
		HRGN doc_region = CreateRectRgn(
			state->frame_rect.left, 
			state->frame_rect.top, 
			state->frame_rect.right, 
			state->frame_rect.bottom);
		CombineRgn(region, region, doc_region, RGN_DIFF);
		FillRgn(hdc, region, background_brush);
		DeleteObject(region);
		DeleteObject(doc_region);
		return TRUE;
	} else if (message == WM_SIZE) {
		gui_update_viewer_layout(state);
		return FALSE;
	} else if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || 
		message == WM_LBUTTONDBLCLK || message == WM_LBUTTONUP || 
		message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK ||
		message == WM_RBUTTONUP) {

		/* Update the splitter state. */
		POINT pos = { short(LOWORD(lp)), short(HIWORD(lp)) };
		if (!state->doc_mouse_capture) {
			if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
				if (PtInRect(&state->hsplitter_box, pos)) {
					state->moving_hsplitter = true;
					SetCursor(LoadCursor(NULL, IDC_SIZEWE));
					SetCapture(hwnd);
					return TRUE;
				} else if (PtInRect(&state->vsplitter_box, pos)) {
					state->moving_vsplitter = true;
					SetCursor(LoadCursor(NULL, IDC_SIZENS));
					SetCapture(hwnd);
					return TRUE;
				}
			} else if (message == WM_LBUTTONUP) {
				if (state->moving_hsplitter || state->moving_vsplitter) {
					ReleaseCapture();
					if (state->moving_hsplitter)
						state->hsplitter_pos = pos.x;
					else
						state->vsplitter_pos = pos.y;
					state->moving_hsplitter = false;
					state->moving_vsplitter = false;
					gui_update_viewer_layout(state);
					return TRUE;
				}
			} else if (message == WM_MOUSEMOVE) {
				if (state->moving_hsplitter) {
					state->hsplitter_pos = pos.x;
					gui_update_viewer_layout(state);
				} else if (state->moving_vsplitter) {
					state->vsplitter_pos = pos.y;
					gui_update_viewer_layout(state);
				} else {
					if (PtInRect(&state->hsplitter_box, pos)) {
						SetCursor(LoadCursor(NULL, IDC_SIZEWE));
					} else if (PtInRect(&state->vsplitter_box, pos)) {
						SetCursor(LoadCursor(NULL, IDC_SIZENS));
					} else {
						SetCursor(LoadCursor(NULL, IDC_ARROW));
					}
				}
			}
		}

		/* Send a mouse message to the document. */
		int x_view = short(LOWORD(lp)) - state->doc_box.left; 
		int y_view = short(HIWORD(lp)) - state->doc_box.top;
		if (PtInRect(&state->doc_box, pos) || state->doc_mouse_capture) {
			MessageType type = MessageType(-1);
			unsigned flags = 0;
			if (message == WM_MOUSEMOVE) {
				type = MSG_MOUSE_MOVE;
			} else {
				if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
					flags |= MMF_SHIFT;
				if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
					flags |= MMF_CTRL;
				if ((GetKeyState(VK_MENU) & 0x8000) != 0)
					flags |= MMF_ALT;
				if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK)
					type = MSG_MOUSE_LEFT_DOWN;
				else if (message == WM_LBUTTONUP)
					type = MSG_MOUSE_LEFT_UP;
				else if (message == WM_RBUTTONDOWN)
					type = MSG_MOUSE_RIGHT_DOWN;
				else if (message == WM_RBUTTONUP)
					type = MSG_MOUSE_RIGHT_UP;
				bool capture = (message == WM_LBUTTONDOWN || 
					message == WM_RBUTTONDOWN);
				if (capture != state->doc_mouse_capture) {
					if (capture) {
						SetCapture(state->dialog_window);
					} else {
						ReleaseCapture();
					}
					state->doc_mouse_capture = capture;
				}
				if (capture)
					SetFocus(hwnd);
			}
			view_handle_mouse_event(state->view, type, x_view, y_view, flags);
			gui_update_cursor(state, get_cursor(state->document));
		}
		return TRUE;
	} else if (message == WM_KEYDOWN || message == WM_KEYUP) {
		MessageType type = MessageType(-1);
		unsigned key_code = 0, flags = 0;
		key_code = MapVirtualKeyA(wp, MAPVK_VK_TO_CHAR);
		if (key_code == 0)
			key_code = wp;
		if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
			flags |= KMF_SHIFT;
		if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
			flags |= KMF_CTRL;
		if ((GetKeyState(VK_MENU) & 0x8000) != 0)
			flags |= KMF_ALT;
		if (message == WM_KEYDOWN) {
			type = MSG_KEY_DOWN;
		} else if (message == WM_KEYUP) {
			type = MSG_KEY_UP;
		}
		view_handle_keyboard_event(state->view, type, key_code, flags);
		return TRUE;
	} else if (message == WM_SETFOCUS) {
		return TRUE;
	} else if (message == WM_VSCROLL) {
		float new_x = state->doc_scroll_x;
		float new_y = state->doc_scroll_y;
		int code = LOWORD(wp);
		int position = HIWORD(wp);
		if (code == SB_THUMBPOSITION || code == SB_THUMBTRACK)
			new_y = (float)position;
		gui_set_scroll_pos(state, new_x, new_y);
		gui_check_paint_clock(state); 
		return FALSE;
	} else if (message == WM_SETCURSOR) {
		POINT cursor_pos;
		GetCursorPos(&cursor_pos);
		ScreenToClient(state->dialog_window, &cursor_pos);
		if (PtInRect(&state->doc_box, cursor_pos)) {
			gui_update_cursor(state, get_cursor(state->document));
			return TRUE;
		}
	}
	return FALSE;
}

static void gui_init(GuiState *state)
{
	state->dialog_window = NULL;
	state->dump_control = NULL;
	state->source_control = NULL;
	state->control_pane_dialog = NULL;
	state->control_group_control = NULL;
	state->edit_proc = NULL;
	state->fixed_font = NULL;
	state->source = NULL;
	state->source_length = 0;
	state->system = NULL;
	state->document = NULL;
	state->view = NULL;
	state->paint_clock = 0;
	state->parse_code = -1;
	state->show_dump_pane = true;
	state->show_navigation_bar = true;
	state->scroll_bar_visible = false;
	state->doc_scroll_x = 0.0f;
	state->doc_scroll_y = 0.0f;
	memset(&state->doc_box, 0, sizeof(state->doc_box));
	state->dump_buffer.push_back('\0');
	state->need_dump_update = false;
	state->moving_hsplitter = false;
	state->moving_vsplitter = false;
	state->hsplitter_pos = -1;
	state->vsplitter_pos = -1;
	state->doc_mouse_capture = false;
	state->signature = 0ULL;
	state->ignore_editor_changes = false;
	state->active_test = GUT_NONE;
	state->test_state = NULL;

	state->url_cache = new UrlCache();
	state->url_cache->set_local_fetch_callback(&local_fetch_callback);

	state->back_end = d2d_init(state->url_cache);
	state->system = create_system(SYSFLAG_TEXT_LAYER_PALETTES, 
		state->back_end, state->url_cache);
	state->document = create_document(state->system, 
		DOCFLAG_ENABLE_SELECTION | 
		DOCFLAG_DEBUG_SELECTION |
		DOCFLAG_EXTERNAL_MESSAGES | 
		DOCFLAG_KEEP_SOURCE);
	state->view = create_view(state->document, 
		VFLAG_DEBUG_OUTER_BOXES | 
		VFLAG_CONSTRAIN_DOCUMENT_WIDTH | 
		VFLAG_DEBUG_MOUSE_HIT);
	state->fixed_font = CreateFont(12 * 96 / 72, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "Consolas");
	state->doc_scroll_x = 0.0f;
	state->doc_scroll_y = 0.0f;

	set_layout_dump_callback(state->document, 
		(DumpCallback)&gui_dump_appendv, state);

	HINSTANCE instance = (HINSTANCE)GetModuleHandle(NULL);
	HWND dialog_window = CreateDialogParam(instance, 
		MAKEINTRESOURCE(IDD_DOCVIEWER), NULL, &gui_dialog_proc,
		(LPARAM)state);
	if (dialog_window == NULL) 
		gui_panic("CreateDialog() failed.");

	state->accelerators = LoadAccelerators(instance, 
		MAKEINTRESOURCE(IDR_ACCELERATORS));

	gui_init_check_boxes(state);
	gui_read_check_boxes(state);
	gui_notify_document_url(state, NULL);
}

static void gui_deinit(GuiState *state)
{
	for (unsigned i = 0; i < state->sample_resource_names.size(); ++i)
		delete [] state->sample_resource_names[i];
	delete [] state->source;
	DeleteObject(state->fixed_font);
	destroy_view(state->view);
	destroy_document(state->document);
	destroy_system(state->system);
	d2d_deinit(state->back_end);
	delete state->url_cache;
}

static bool gui_message_loop(GuiState *state)
{
	MSG message;
	for (;;) {
		if (PeekMessage(&message, NULL, 0, 0, PM_REMOVE)) {
			if (message.message == WM_QUIT)
				return false;
			if (TranslateAccelerator(state->dialog_window, 
				state->accelerators, &message))
				continue;
			if (message.message != WM_KEYDOWN && 
				message.message != WM_KEYUP &&
				message.message != WM_CHAR &&
				IsDialogMessage(state->dialog_window, &message))
				continue;
			TranslateMessage(&message);
			DispatchMessage(&message);
		} else {
			bool idle = true;
			state->url_cache->update();
			if (state->need_dump_update) {
				gui_dump_update(state);
				idle = false;
			}
			gui_update_test(state);
			gui_handle_document_messages(state);
			if (gui_check_paint_clock(state))
				idle = false;
			if (idle)
				break;
		}
	}
	return true;
}

void ide(void)
{
 	InitCommonControls();
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	Scintilla_RegisterClasses(GetModuleHandle(NULL));
	GuiState *state = new GuiState();
	gui_init(state);
	ShowWindow(state->dialog_window, SW_SHOW);
	while (gui_message_loop(state))
		Sleep(10);
	gui_deinit(state);
	delete state;
}

} // namespace stkr

int main(int argc, char *argv[])
{
	argc; argv;
	stkr::ide();

	//urlcache::unit_test();
	//getchar();

	//sstl::unit_test();
	//getchar();

	return 0;
}

#endif // defined(STACKER_IDE)
