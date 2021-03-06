/* 
 * Copyright (c) 2015-2016, Gregory M. Kurtzer. All rights reserved.
 * 
 * “Singularity” Copyright (c) 2016, The Regents of the University of California,
 * through Lawrence Berkeley National Laboratory (subject to receipt of any
 * required approvals from the U.S. Dept. of Energy).  All rights reserved.
 * 
 * If you have questions about your rights to use or distribute this software,
 * please contact Berkeley Lab's Innovation & Partnerships Office at
 * IPO@lbl.gov.
 * 
 * NOTICE.  This Software was developed under funding from the U.S. Department of
 * Energy and the U.S. Government consequently retains certain rights. As such,
 * the U.S. Government has been granted for itself and others acting on its
 * behalf a paid-up, nonexclusive, irrevocable, worldwide license in the Software
 * to reproduce, distribute copies to the public, prepare derivative works, and
 * perform publicly and display publicly, and to permit other to do so. 
 * 
 */


char *file_id(char *path);
int is_file(char *path);
int is_link(char *path);
int is_dir(char *path);
int is_exec(char *path);
int is_owner(char *path, uid_t uid);
int is_blk(char *path);
int s_mkpath(char *dir, mode_t mode);
int s_rmdir(char *dir);
int intlen(int input);
char *int2str(int num);
int copy_file(char * source, char * dest);
char *joinpath(char * path1, char * path2);
char *strjoin(char *str1, char *str2);
char *random_string(int length);
char *filecat(char *path);
int fileput(char *path, char *string);
