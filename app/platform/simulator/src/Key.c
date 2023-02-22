/**
 * SSAS - Simple Smart Automotive Software
 * Copyright (C) 2021 Parai Wang <parai@foxmail.com>
 */
/* ================================ [ INCLUDES  ] ============================================== */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include "Sd.h"

#ifdef _WIN32
#include <windows.h>
#endif
/* ================================ [ MACROS    ] ============================================== */
/* ================================ [ TYPES     ] ============================================== */
/* ================================ [ DECLARES  ] ============================================== */
/* ================================ [ DATAS     ] ============================================== */
static pthread_t lThread;
/* ================================ [ LOCALS    ] ============================================== */
#ifdef _WIN32
static int _getch(void) {
  int ch = -1;
  int i;
  SHORT state;

  static const char keys[] = "SK1234567890QWERTYUIOPXD";
  static uint64_t KeyFlag = 0x00;

  for (i = 0; i < sizeof(keys); i++) {
    state = GetKeyState((int)keys[i]);
    if (0x80 & state) {
      if (0 == (KeyFlag & (1 << i))) {
        ch = keys[i];
        KeyFlag |= 1 << i;
        break;
      }
    } else {
      KeyFlag &= ~(1 << i);
    }
  }

  if (-1 == ch) {
    usleep(10000);
  } else {
    if ((ch >= 'A') && (ch <= 'Z')) {
      ch = 'a' + ch - 'A';
    }
  }

  return ch;
}
#endif

static void *KeyMonitorThread(void *arg) {
  printf("press key <s> to toggle SD start/top\n");
  while (TRUE) {
#ifdef _WIN32
    int ch = _getch();
#else
    int ch = getchar();
#endif
    (void)ch;
    if (ch == 's') {
      static int avtive = FALSE;
      if (FALSE == avtive) {
        printf("SD request\n");
        Sd_ServerServiceSetState(0, SD_SERVER_SERVICE_AVAILABLE);
        Sd_ClientServiceSetState(0, SD_CLIENT_SERVICE_REQUESTED);
        Sd_ConsumedEventGroupSetState(0, SD_CONSUMED_EVENTGROUP_REQUESTED);
        avtive = TRUE;
      } else {
        printf("SD release\n");
        Sd_ServerServiceSetState(0, SD_SERVER_SERVICE_DOWN);
        Sd_ClientServiceSetState(0, SD_CLIENT_SERVICE_RELEASED);
        Sd_ConsumedEventGroupSetState(0, SD_CONSUMED_EVENTGROUP_RELEASED);
        avtive = FALSE;
      }
    }
  }

  return NULL;
}

static void __attribute__((constructor)) _key_mgr_start(void) {
  pthread_create(&lThread, NULL, KeyMonitorThread, NULL);
}
/* ================================ [ FUNCTIONS ] ============================================== */
