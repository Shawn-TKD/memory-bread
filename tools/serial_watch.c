#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;

static void stop_watching(int signal_number) {
  (void)signal_number;
  keep_running = 0;
}

static void stamp(const char *message) {
  time_t now = time(NULL);
  struct tm local;
  localtime_r(&now, &local);
  char value[32];
  strftime(value, sizeof(value), "%H:%M:%S", &local);
  fprintf(stderr, "[%s] %s\n", value, message);
  fflush(stderr);
}

static int open_serial(const char *path) {
  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) return -1;

  struct termios options;
  if (tcgetattr(fd, &options) != 0) {
    close(fd);
    return -1;
  }
  cfmakeraw(&options);
  cfsetspeed(&options, B115200);
  options.c_cflag |= CLOCAL | CREAD;
  options.c_cflag &= ~HUPCL;
  if (tcsetattr(fd, TCSANOW, &options) != 0) {
    close(fd);
    return -1;
  }

  // Arduino USB CDC only emits Serial output while the host asserts DTR.
  // Keep RTS explicitly deasserted so we never present the DTR+RTS reset
  // combination used by automatic flashing tools. No bytes are written.
  ioctl(fd, TIOCSDTR);
  int clear_bits = TIOCM_RTS;
  ioctl(fd, TIOCMBIC, &clear_bits);
  return fd;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s /dev/cu.usbmodem...\n", argv[0]);
    return 2;
  }

  signal(SIGINT, stop_watching);
  signal(SIGTERM, stop_watching);

  int fd = -1;
  int was_connected = 0;
  unsigned char buffer[512];
  while (keep_running) {
    if (fd < 0) {
      fd = open_serial(argv[1]);
      if (fd < 0) {
        if (was_connected) {
          stamp("serial port disappeared; waiting for re-enumeration");
          was_connected = 0;
        }
        usleep(200000);
        continue;
      }
      stamp(was_connected ? "serial port reopened" : "serial port opened (read-only)");
      was_connected = 1;
    }

    ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      fwrite(buffer, 1, (size_t)count, stdout);
      fflush(stdout);
    } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      close(fd);
      fd = -1;
    } else {
      usleep(20000);
    }
  }

  if (fd >= 0) close(fd);
  return 0;
}
