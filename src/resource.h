#ifndef CLAUDE_DESKTOP_PROFILES_MANAGER_RESOURCE_H
#define CLAUDE_DESKTOP_PROFILES_MANAGER_RESOURCE_H

#define IDI_APP           1

/* The manager window's class: IDD_MAIN's in app.rc, the one the program registers and finds. */
#define APP_WINDOW_CLASS_NAME "ClaudeDesktopProfilesManagerMain"
#define APP_WINDOW_CLASS      L"" APP_WINDOW_CLASS_NAME

#define IDD_MAIN          100
#define IDD_PROFILE       101
#define IDD_UNINSTALL     102
#define IDD_MESSAGE       103
#define IDD_TITLE         104

/* main window */
#define IDC_STATUS        1000
#define IDC_STATUS_ACTION 1001
#define IDC_LIST          1002
#define IDC_OPEN          1003
#define IDC_NEW           1004
#define IDC_EDIT          1005
#define IDC_DELETE        1006
#define IDC_DEFAULT       1007
#define IDC_NOTE          1008
#define IDC_SC_GROUP      1009
#define IDC_SC_DESKTOP    1010
#define IDC_SC_SAVEAS     1011
#define IDC_SC_PIN        1012
#define IDC_UNINSTALL     1013
#define IDC_ABOUT         1014
#define IDC_UPDATE        1015
#define IDC_SC_START      1016
#define IDC_SESSIONS      1017
#define IDC_LANGUAGE      1018

/* sessions view of the main window */
#define IDC_S_SEARCH      1400
#define IDC_S_ARCHIVED    1401
#define IDC_S_PROFILES    1402
#define IDC_S_TREE        1403
#define IDC_S_DETAILS     1404
#define IDC_S_PARTS       1405

/* session title dialog */
#define IDC_T_LABEL       1500
#define IDC_T_TITLE       1501

/* profile dialog */
#define IDC_P_NAME        1100
#define IDC_P_COLOR       1101
#define IDC_P_PREVIEW     1102
#define IDC_P_FOLDER      1103
#define IDC_P_ERROR       1104
#define IDC_P_OPEN        1105
#define IDC_P_STARTUP     1106
#define IDC_P_COPY        1107

/* uninstall dialog */
#define IDC_U_KEEP        1200
#define IDC_U_LABEL       1201
#define IDC_U_LIST        1202
#define IDC_U_HINT        1203

/* message box */
#define IDC_M_ICON        1300
#define IDC_M_TEXT        1301

#endif
