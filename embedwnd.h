#ifndef __EMBEDWND_H
#define __EMBEDWND_H

#include <winamp/gen.h>
#include <winamp/wa_ipc.h>
#include "api.h"

#ifndef WINAMP_REFRESHSKIN
#define WINAMP_REFRESHSKIN 40291
#endif

// you can change these values for the section name to store/access the settings
// from winamp.ini and also for the property set when starting Winamp minimised.
#define INI_FILE_SECTION L"Waveseek"
#define MINIMISED_FLAG L"WFSMinMode"

// these functions deal with creating the embedded window and relevant menus, etc
HWND CreateEmbeddedWindow(embedWindowState* embedWindow, const GUID embedWindowGUID);
void DestroyEmbeddedWindow(embedWindowState* embedWindow);
/*void AddEmbeddedWindowToMenus(const BOOL add, const UINT menuId, LPCWSTR menuString, const BOOL setVisible);
void UpdateEmbeddedWindowsMenu(const UINT menuId);
bool SetEmbeddedWindowMinimisedMode(HWND embeddedWindow, const bool minimised);
bool EmbeddedWindowIsMinimisedMode(HWND embeddedWindow);*/

// these functions are used to process any relevant menu or window messages which the
// embedded window needs to detect inorder to work (especially betweeen instances)
LRESULT HandleEmbeddedWindowChildMessages(HWND embedWnd, UINT menuId, HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
void HandleEmbeddedWindowWinampWindowMessages(HWND embedWnd, UINT_PTR menuId, embedWindowState* embedWindow,
											  HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

// shared variables with the embedwnd code this can be altered if you want to use
// different variables or means but is done like this to simplify this example
extern winampGeneralPurposePlugin plugin;
extern BOOL visible, old_visible;

#endif