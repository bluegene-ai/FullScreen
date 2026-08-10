#pragma once

// ============================================================
// FullScreen Browser - Resource IDs
// ============================================================

#include "version.h"

#ifndef IDC_STATIC
#define IDC_STATIC              (-1)
#endif

// Dialog IDs
#define IDD_CONFIG_DIALOG       1001
#define IDD_PASSWORD_DIALOG     1002
#define IDD_MENU_DIALOG         1003
#define IDD_REGISTER_DIALOG     1004

// Control IDs - Config Dialog
#define IDC_URL                 2001
#define IDC_PASSWORD            2002
#define IDC_CONFIRM_PASSWORD    2003
#define IDC_UNREACHABLE_MSG     2004
#define IDC_ZOOM                2005
#define IDC_ZOOM_SPIN           2006
#define IDC_URL_STATUS          2007
#define IDC_BTN_TEST_URL        2008
#define IDC_REFRESH_INTERVAL    2009
#define IDC_BURNIN_CHECK        2010
#define IDC_RADIO_REFRESH_OFF       2011
#define IDC_RADIO_REFRESH_INTERVAL  2012
#define IDC_RADIO_REFRESH_DAILY     2013
#define IDC_REFRESH_DAILY           2014
#define IDC_REMOTE_ENABLE           2015
#define IDC_REMOTE_BASE_URL         2016
#define IDC_AUTOSTART_CHECK         2017
#define IDC_AUTO_UPDATE_CHECK       2018
#define IDC_RADIO_UPDATE_GITHUB     2019
#define IDC_RADIO_UPDATE_SELF       2020
#define IDC_UPDATE_REPO             2021
#define IDC_UPDATE_BASE_URL         2022
#define IDC_UPDATE_WINDOW           2023

// Control IDs - Password Dialog
#define IDC_PWD_INPUT           3001

// Control IDs - Menu Dialog
#define IDC_BTN_EXIT            4001
#define IDC_BTN_SETTINGS        4002
#define IDC_BTN_CANCEL          4003

// Control IDs - Register Dialog
#define IDC_REGISTER_CODE       4501

// Timer IDs
#define TIMER_CURSOR_HIDE       5002
#define TIMER_PIXEL_SHIFT       5003
#define TIMER_AUTO_REFRESH      5004

// Icon ID
#define IDI_MAIN_ICON           6001

// Version (defined in version.h, injected by the build pipeline)
