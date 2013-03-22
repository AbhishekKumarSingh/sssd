/*
    SSSD

    find_uid - Utilities tests

    Authors:
        Abhishek Singh <abhishekkumarsingh.cse@gmail.com>

    Copyright (C) 2013 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <dirent.h>
#include <unistd.h>

#include "util/io.h"

#define FILE_PATH "/tmp/test.in"
#define NON_EX_PATH "non-existent-path"

static void setup_all(void)
{
    FILE *fp;
    fp = fopen(FILE_PATH, "w");
    if (fp != NULL)
        fclose(fp);
    else
        printf("Error! file test.in can't be created");
}

static void teardown_all(void)
{
    remove(FILE_PATH);
}

static int get_dirfd(void)
{
    int dir_fd;
    DIR *tmp = opendir("/tmp");
        if (tmp != NULL)
            dir_fd = dirfd(tmp);

    return dir_fd;
}

void test_sss_open_cloexec_success(void **state)
{
    int fd;
    int ret;
    int ret_flag;
    int expec_flag;
    int flags = O_RDWR;

    fd = sss_open_cloexec(FILE_PATH, flags, &ret);

    ret_flag = fcntl(fd, F_GETFD, 0);
    expec_flag = FD_CLOEXEC;

    assert_true(fd != -1);
    assert_true(ret_flag & expec_flag);

    close(fd);
}

void test_sss_open_cloexec_fail(void **state)
{
    int fd;
    int ret;
    int flags = O_RDWR;

    fd = sss_open_cloexec(NON_EX_PATH, flags, &ret);

    assert_true(fd == -1);
    assert_int_not_equal(ret, 0);

    close(fd);
}

void test_sss_openat_cloexec_success(void **state)
{
    int fd;
    int ret;
    int ret_flag;
    int expec_flag;
    int dir_fd;
    int flags = O_RDWR;

    dir_fd = get_dirfd();
    fd = sss_openat_cloexec(dir_fd, "test.in", flags, &ret);

    ret_flag = fcntl(fd, F_GETFD, 0);
    expec_flag = FD_CLOEXEC;

    assert_true(fd != -1);
    assert_true(ret_flag & expec_flag);

    close(fd);
}

void test_sss_openat_cloexec_fail(void **state)
{
    int fd;
    int ret;
    int dir_fd;
    int flags = O_RDWR;

    dir_fd = get_dirfd();
    fd = sss_openat_cloexec(dir_fd, NON_EX_PATH, flags, &ret);

    assert_true(fd == -1);
    assert_int_not_equal(ret, 0);

    close(fd);
}

int main(void)
{
    const UnitTest tests[] = {
        unit_test_setup_teardown(test_sss_open_cloexec_success, setup_all,
                                 teardown_all),
        unit_test_setup_teardown(test_sss_open_cloexec_fail, setup_all,
                                 teardown_all),
        unit_test_setup_teardown(test_sss_openat_cloexec_success, setup_all,
                                 teardown_all),
        unit_test_setup_teardown(test_sss_openat_cloexec_fail, setup_all,
                                 teardown_all)
    };

    return run_tests(tests);
}
