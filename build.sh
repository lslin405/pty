#!/bin/bash

gcc cli.c -o cli
gcc pty.c pty_fork.c pty_master_open.c -o pty
