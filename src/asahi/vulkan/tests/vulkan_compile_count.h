/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Counts the shaders the driver compiles while a test records a command
 * buffer, for tests that check a warm disk cache keeps compiles off the draw
 * path. A count does not depend on how fast the machine happens to be.
 *
 * With AGX_MESA_DEBUG=shaderdb, agx_compile_shader_nir() prints one
 * "<STAGE> shader: ..." statistics line to stderr for every shader it
 * compiles, except internal ones. Prologs, epilogs and meta shaders are built
 * with nir_builder_init_simple_shader(), which marks them internal, so they
 * print nothing. A draw-time build from the application's shader does print:
 * a multisample variant, a border-emulating twin, or the G17P inline-blend
 * program. Loading any of those from the disk cache compiles nothing, so it
 * prints nothing.
 *
 * The flag is part of the driver's cache identity (hk_codegen_env_hash()), so
 * call compile_count_enable() before the first Vulkan instance and keep it set
 * for every instance that shares the cache.
 */
#ifndef VULKAN_COMPILE_COUNT_H
#define VULKAN_COMPILE_COUNT_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void compile_count_enable(void) {
  const char *old = getenv("AGX_MESA_DEBUG");
  char flags[256];
  snprintf(flags, sizeof(flags), "%s%sshaderdb", old ? old : "",
           old && *old ? "," : "");
  setenv("AGX_MESA_DEBUG", flags, 1);
}

struct compile_count {
  FILE *file;
  int saved_stderr;
};

static struct compile_count *compile_count_active;
static unsigned compile_count_end(struct compile_count *c);

/* A test that exits while capturing still shows why. */
static void compile_count_at_exit(void) {
  if (compile_count_active)
    compile_count_end(compile_count_active);
}

/* Sends stderr to a temporary file until compile_count_end(). */
static void compile_count_begin(struct compile_count *c) {
  static bool registered;
  if (!registered && atexit(compile_count_at_exit) != 0) {
    perror("atexit");
    exit(2);
  }
  registered = true;
  fflush(stderr);
  c->file = tmpfile();
  c->saved_stderr = dup(STDERR_FILENO);
  if (!c->file || c->saved_stderr < 0 ||
      dup2(fileno(c->file), STDERR_FILENO) < 0) {
    perror("capturing stderr");
    exit(2);
  }
  compile_count_active = c;
}

static bool compile_count_is_stats_line(const char *line) {
  const char *p = line;
  while (*p >= 'A' && *p <= 'Z')
    p++;
  return p != line && !strncmp(p, " shader: ", 9);
}

/* Restores stderr, copies what was captured to it, and returns the number of
 * shaders compiled in between. */
static unsigned compile_count_end(struct compile_count *c) {
  compile_count_active = NULL;
  fflush(stderr);
  if (dup2(c->saved_stderr, STDERR_FILENO) < 0) {
    perror("restoring stderr");
    exit(2);
  }
  close(c->saved_stderr);
  rewind(c->file);

  unsigned count = 0;
  bool line_start = true;
  char buf[512];
  while (fgets(buf, sizeof(buf), c->file)) {
    if (line_start && compile_count_is_stats_line(buf))
      count++;
    fputs(buf, stderr);
    line_start = strchr(buf, '\n') != NULL;
  }
  fclose(c->file);
  return count;
}

#endif
