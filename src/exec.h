/*
** Copyright (C) 2002 Christophe Kalt
**
** This file is part of shmux
** see the LICENSE file for details on your rights.
**
** $Id: exec.h,v 1.1 2002-07-04 21:44:49 kalt Exp $
*/

#if !defined(_EXEC_H_)
# define _EXEC_H_

pid_t exec(int *, int *, int *, char *, char **, int);

#endif