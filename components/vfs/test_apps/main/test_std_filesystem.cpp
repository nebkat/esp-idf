/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <filesystem>
#include <cstring>
#include <sys/errno.h>
#include <sys/stat.h>
#include "esp_vfs.h"
#include "unity.h"

/* Helper VFS driver to test std::filesystem */

typedef struct {
    const char* cmp_path;
    int ret_errno;
    struct stat ret_stat;
} test_vfs_ctx_t;


static int test_vfs_open(void* ctx, const char* path, int flags, int mode)
{
    test_vfs_ctx_t* vfs_ctx = (test_vfs_ctx_t*)ctx;
    if (strcmp(path, vfs_ctx->cmp_path) != 0) {
        errno = vfs_ctx->ret_errno;
        return -1;
    }
    return 0;
}

static int test_vfs_stat(void* ctx, const char* path, struct stat* st)
{
    test_vfs_ctx_t* vfs_ctx = (test_vfs_ctx_t*)ctx;
    if (strcmp(path, vfs_ctx->cmp_path) != 0) {
        errno = vfs_ctx->ret_errno;
        return -1;
    }
    *st = vfs_ctx->ret_stat;
    return 0;
}

static void test_vfs_setup(test_vfs_ctx_t* ctx)
{
    esp_vfs_t desc = {
        .flags = ESP_VFS_FLAG_CONTEXT_PTR,
        .open_p = test_vfs_open,
        .stat_p = test_vfs_stat,
    };
    TEST_ESP_OK(esp_vfs_register("/test", &desc, ctx));
}

static void test_vfs_teardown()
{
    TEST_ESP_OK(esp_vfs_unregister("/test"));
}

/* Actual std::filesystem tests start below */


TEST_CASE("std::filesystem path, relative, absolute", "[std_filesystem]")
{
    // In IDF, CWD is always in the the root directory
    TEST_ASSERT_MESSAGE(std::filesystem::current_path() == "/", "CWD should be root directory");

    // Create absolute path from relative path
    std::filesystem::path rel_path("test/file.txt");
    std::filesystem::path abs_path = std::filesystem::absolute(rel_path);
    TEST_ASSERT_EQUAL_STRING("/test/file.txt", abs_path.c_str());

    // Create relative path from absolute path
    std::filesystem::path rel_path2 = std::filesystem::relative(abs_path);
    TEST_ASSERT_EQUAL_STRING("test/file.txt", rel_path2.c_str());

    // Create relative path from absolute path with different base
    std::filesystem::path rel_path3 = std::filesystem::relative(abs_path, "/test");
    TEST_ASSERT_EQUAL_STRING("file.txt", rel_path3.c_str());
}

TEST_CASE("std::filesystem exists function", "[std_filesystem]")
{
    test_vfs_ctx_t test_ctx = {};

    test_vfs_setup(&test_ctx);

    // Test file exists
    test_ctx.cmp_path = "/file.txt";
    test_ctx.ret_stat = (struct stat){.st_mode = S_IFREG};
    try {
        TEST_ASSERT_MESSAGE(std::filesystem::exists("/test/file.txt"), "File should exist");

        TEST_ASSERT_MESSAGE(std::filesystem::is_regular_file("/test/file.txt"), "Path should be a file");
    } catch (std::exception &e) {
        TEST_FAIL_MESSAGE(e.what());
    }

    // Test directory exists
    test_ctx.cmp_path = "/dir";
    test_ctx.ret_stat = (struct stat){.st_mode = S_IFDIR};
    TEST_ASSERT_MESSAGE(std::filesystem::exists("/test/dir"), "Directory should exist");
    TEST_ASSERT_MESSAGE(std::filesystem::is_directory("/test/dir"), "Path should be a directory");

    // Test non-existent path
    test_ctx.cmp_path = "";
    test_ctx.ret_errno = ENOENT;
    TEST_ASSERT_MESSAGE(!std::filesystem::exists("/test/nonexistent"), "Path should not exist");

    test_vfs_teardown();
}
