#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>
#include "archive.h"
#include "dxt.h"

// Control IDs
#define IDM_FILE_OPEN       101
#define IDM_FILE_EXIT       102
#define IDM_VIEW_RGBA       201
#define IDM_VIEW_RGB        202
#define IDM_VIEW_ALPHA      203
#define IDM_HELP_ABOUT      301
#define IDM_HELP_UPDATE     302
#define IDM_TEX_REPLACE     401
#define IDM_TEX_EXPORT      402

#define IDC_LISTVIEW        1001
#define IDC_SEARCH          1002
#define IDC_PREVIEW_PANEL   1003
#define IDC_INFO_LABEL      1004
#define IDC_STATUS_BAR      1005

// Splitter width in pixels
#define SPLITTER_W 5

struct GuiState
{
    HWND        hwndMain;
    HWND        hwndList;
    HWND        hwndSearch;
    HWND        hwndPreview;
    HWND        hwndInfo;
    HWND        hwndStatus;

    Archive     arc;
    bool        arcLoaded;

    int         selectedIdx;
    ChannelMode chanMode;

    HBITMAP     hbmPreview;
    int         texW, texH; 

    int         splitterX; 
    bool        draggingSplit;
};

bool GuiInit(HINSTANCE hInst, int nCmdShow);
int  GuiRun();

LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK PreviewWndProc(HWND, UINT, WPARAM, LPARAM);

void GuiOpenArchive(HWND hwnd, const std::wstring& path = {});

void GuiSelectTexture(int idx);

void GuiReplaceTexture(HWND hwnd);

void GuiExportTexture(HWND hwnd);

void GuiRefreshList(const std::wstring& filter = {});

void GuiSetStatus(const wchar_t* fmt, ...);

HWND GuiGetMainHwnd();
