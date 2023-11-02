#ifndef _PTY_MASTER_OPEN_H_
#define _PTY_MASTER_OPEN_H_

#include <stdlib.h>

int ptyMasterOpen(char *slaveName, size_t snLen);

#endif
