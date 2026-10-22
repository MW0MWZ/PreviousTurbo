/*
  Previous - dialog.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#ifndef PREV_DIALOG_H
#define PREV_DIALOG_H

#include "configuration.h"

/* provided by the window's dialogs (gui/): */
extern int  Dialog_MainDlg(bool *bReset, bool *bLoadedSnapshot);
extern bool DlgAlert_Notice(const char *text);
extern bool DlgAlert_Query(const char *text);
extern void Dialog_GraphicsCheckConsole(int board);
extern void DlgMissing_Rom(const char* type, char *imgname, const char *defname, bool *enabled);
extern void DlgMissing_Dir(const char* type, char *dirname, const char *defname);
extern void DlgMissing_Disk(const char* type, int num, char *imgname, bool *ins, bool *wp);
/* and dialog.c */
extern bool Dialog_DoProperty(void);
extern void Dialog_CheckFiles(void);

#endif /* PREV_DIALOG_H */
