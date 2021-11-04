#include <windows.h>
#include <strsafe.h>
#include "embedwnd.h"
#include <winamp/wa_cup.h>
#include <loader/loader/utils.h>
#include <loader/loader/ini.h>

// internal variables
HMENU main_menu = 0, windows_menu = 0;
int height = 0, width = 0;
BOOL visible = FALSE, old_visible = FALSE,
	 self_update = FALSE;
RECT initial[2] = {0};

HWND CreateEmbeddedWindow(embedWindowState* embedWindow, const GUID embedWindowGUID)
{
	// this sets a GUID which can be used in a modern skin / other parts of Winamp to
	// indentify the embedded window frame such as allowing it to activated in a skin
	SET_EMBED_GUID((embedWindow), embedWindowGUID);

	// when creating the frame it is easier to use Winamp's handling to specify the
	// position of the embedded window when it is created saving addtional handling
	//
	// how you store the settings is down to you, this example uses winamp.ini for ease
	embedWindow->r.left = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"PosX", 275);
	embedWindow->r.top = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"PosY", 348);
	
	//TODO map from the old values?
	int right = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"wnd_right", -1),
		bottom = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"wnd_bottom", -1);

	if (right != -1)
	{
		embedWindow->r.right = right;
		SaveNativeIniString(WINAMP_INI, INI_FILE_SECTION, L"wnd_right", 0);
	}
	else
	{
		embedWindow->r.right = embedWindow->r.left + GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"SizeX", 500);
	}

	if (bottom != -1)
	{
		embedWindow->r.bottom = bottom;
		SaveNativeIniString(WINAMP_INI, INI_FILE_SECTION, L"wnd_bottom", 0);
	}
	else
	{
		embedWindow->r.bottom = embedWindow->r.top + GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"SizeY", 87);
	}

	CopyRect(&initial[0], &embedWindow->r);

	initial[1].top = height = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"ff_height", height);
	initial[1].left = width = GetNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"ff_width", width);

	// specifying this will prevent the modern skin engine (gen_ff) from adding a menu entry
	// to the main right-click menu. this is useful if you want to add your own menu item so
	// you can show a keyboard accelerator (as we are doing) without a generic menu added
	embedWindow->flags |= EMBED_FLAGS_NOWINDOWMENU;

	// now we have set up the embedWindowState structure, we pass it to Winamp to create
	return plugin.createembed(embedWindow);
}

void DestroyEmbeddedWindow(embedWindowState* embedWindow)
{
	// unless we're closing as a classic skin then we'll
	// skip saving the current window position otherwise
	// we have the issue with windows being in the wrong
	// places after modern -> exit -> modern -> classic
	if (!embedWindow->wasabi_window &&
		!EqualRect(&initial[0], &embedWindow->r))
	{
		SaveNativeIniInt(WINAMP_INI, INI_FILE_SECTION,
						 L"PosX", embedWindow->r.left);
		SaveNativeIniInt(WINAMP_INI, INI_FILE_SECTION,
						 L"PosY", embedWindow->r.top);
		SaveNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"SizeX",
						 (embedWindow->r.right - embedWindow->r.left));
		SaveNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"SizeY",
						 (embedWindow->r.bottom - embedWindow->r.top));
	}

	if (old_visible != visible)
	{
		SaveNativeIniString(WINAMP_INI, INI_FILE_SECTION,
							L"wnd_open", (!visible ? L"0" : NULL));
	}

	if (initial[1].top != height || initial[1].left != width)
	{
		SaveNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"ff_height", height);
		SaveNativeIniInt(WINAMP_INI, INI_FILE_SECTION, L"ff_width", width);
	}
}

void AddEmbeddedWindowToMenus(BOOL add, UINT menuId, LPWSTR menuString, BOOL setVisible)
{
	// this will add a menu item to the main right-click menu
	if (add)
	{
		main_menu = GetNativeMenu((WPARAM)0);

		int prior_item = GetMenuItemID(main_menu, 9);
		if (prior_item <= 0)
		{
			prior_item = GetMenuItemID(main_menu, 8);
		}
		AddItemToMenu2(main_menu, menuId, menuString, prior_item, 0);
		CheckMenuItem(main_menu, menuId, MF_BYCOMMAND |
					  ((setVisible == -1 ? visible : setVisible) ? MF_CHECKED : MF_UNCHECKED));
	}
	else
	{
		DeleteMenu(main_menu, menuId, MF_BYCOMMAND);
	}

#ifdef IPC_ADJUST_OPTIONSMENUPOS
	// this will adjust the menu position (there were bugs with this api but all is fine for 5.5+)
	// cppcheck-suppress ConfigurationNotChecked	
	SendMessage(plugin.hwndParent, WM_WA_IPC, (add ? 1 : -1), IPC_ADJUST_OPTIONSMENUPOS);
#endif

	// this will add a menu item to the main window views menu
	if (add)
	{
		windows_menu = GetNativeMenu((WPARAM)4);

		int prior_item = GetMenuItemID(windows_menu, 3);
		if (prior_item <= 0)
		{
			prior_item = GetMenuItemID(windows_menu, 2);
		}

		AddItemToMenu2(windows_menu, menuId, menuString, prior_item, 0);
		CheckMenuItem(windows_menu, menuId, MF_BYCOMMAND |
					  ((setVisible == -1 ? visible : setVisible) ? MF_CHECKED : MF_UNCHECKED));
	}
	else
	{
		DeleteMenu(windows_menu,menuId,MF_BYCOMMAND);
	}

#ifdef IPC_ADJUST_FFWINDOWSMENUPOS
	// this will adjust the menu position (there were bugs with this api but all is fine for 5.5+)
	// cppcheck-suppress ConfigurationNotChecked
	SendMessage(plugin.hwndParent, WM_WA_IPC, (add ? 1 : -1), IPC_ADJUST_FFWINDOWSMENUPOS);
#endif
}

void UpdateEmbeddedWindowsMenu(UINT menuId)
{
	const UINT check = MF_BYCOMMAND | (visible ? MF_CHECKED : MF_UNCHECKED);
	if (main_menu)
	{
		CheckMenuItem(main_menu, menuId, check);
	}
	if (windows_menu)
	{
		CheckMenuItem(windows_menu, menuId, check);
	}
}

BOOL SetEmbeddedWindowMinimizedMode(HWND embeddedWindow, BOOL fMinimized)
{
	if (fMinimized == TRUE)
	{
		return SetProp(embeddedWindow, MINIMISED_FLAG, (HANDLE)1);
	}	
	RemoveProp(embeddedWindow, MINIMISED_FLAG);
	return TRUE;
}

BOOL EmbeddedWindowIsMinimizedMode(HWND embeddedWindow)
{
	return (GetProp(embeddedWindow, MINIMISED_FLAG) != 0);
}

LRESULT HandleEmbeddedWindowChildMessages(HWND embedWnd, UINT menuId, HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	// we handle both messages so we can get the action when sent via the keyboard
	// shortcut but also copes with using the menu via Winamp's taskbar system menu
	if ((message == WM_SYSCOMMAND || message == WM_COMMAND) && LOWORD(wParam) == menuId)
	{
		self_update = TRUE;
		PostMessage(embedWnd, WM_USER + (!IsWindowVisible(embedWnd) ? 102 : 105), 0, 0);
		visible = !visible;
		UpdateEmbeddedWindowsMenu(menuId);
		self_update = FALSE;
		return 1;
	}
	// this is sent to the child window of the frame when the 'close' button is clicked
	else if (message == WM_CLOSE)
	{
		PostMessage(embedWnd, WM_USER + 105, 0, 0);
		visible = 0;
		UpdateEmbeddedWindowsMenu(menuId);
		PostMessage(plugin.hwndParent, WM_COMMAND, MAKEWPARAM(WINAMP_NEXT_WINDOW, 0), 0);
	}
	else if (message == WM_WINDOWPOSCHANGING)
	{
		/*
		 if extra_data[EMBED_STATE_EXTRA_REPARENTING] is set, we are being reparented by the freeform lib, so we should
		 just ignore this message because our visibility will not change once the freeform
		 takeover/restoration is complete
		*/
		embedWindowState *state=(embedWindowState *)GetWindowLongPtr(embedWnd,GWLP_USERDATA);
		if (state && state->reparenting && !GetParent(embedWnd))
		{
			// this will reset the position of the frame when we need it to
			// usually from going classic->modern->close->start->classic
			SetWindowPos(embedWnd, 0, 0, 0, width, height,
						 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE |
						 SWP_NOSENDCHANGING | SWP_ASYNCWINDOWPOS);
		}
	}
	return 0;
}

void HandleEmbeddedWindowWinampWindowMessages(HWND embedWnd, UINT menuId, embedWindowState* embedWindow,
											  HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	// these are done before the original window proceedure has been called to
	// ensure we get the correct size of the window and for checking the menu
	// item for the embedded window as applicable
	if (message == WM_SYSCOMMAND || message == WM_COMMAND)
	{
		if (LOWORD(wParam) == menuId)
		{
			self_update = TRUE;
			PostMessage(embedWnd, WM_USER + (!IsWindowVisible(embedWnd) ? 102 : 105), 0, 0);
			visible = !visible;
			UpdateEmbeddedWindowsMenu(menuId);
			self_update = FALSE;
		}
		else if (LOWORD(wParam) == WINAMP_REFRESHSKIN)
		{
			if (!GetParent(embedWnd))
			{
				width = (embedWindow->r.right - embedWindow->r.left);
				height = (embedWindow->r.bottom - embedWindow->r.top);
			}
		}
	}
	else if (message == WM_WA_IPC)
	{
		if (lParam == IPC_SKIN_CHANGED_NEW)
		{
			PostMessage(GetWindow(embedWnd, GW_CHILD), WM_USER + 0x202, 0, 0);
		}
		else if ((lParam == IPC_CB_ONSHOWWND) || (lParam == IPC_CB_ONHIDEWND))
		{
			if (((HWND)wParam == embedWnd) && !self_update)
			{
				visible = (lParam == IPC_CB_ONSHOWWND);
				UpdateEmbeddedWindowsMenu(menuId);
			}
		}
		else if ((lParam == IPC_IS_MINIMISED_OR_RESTORED) && !wParam)
		{
			// this is used to cope with Winamp being started minimised and will then
			// re-show the example window when Winamp is being restored to visibility
			if (EmbeddedWindowIsMinimizedMode(embedWnd))
			{
				PostMessage(embedWnd, WM_USER + (visible ? 102 : 105), 0, 0);
				SetEmbeddedWindowMinimizedMode(embedWnd, FALSE);
			}
		}
	}
}