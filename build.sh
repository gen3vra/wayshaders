wayland-scanner client-header \
  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
  xdg-shell.h

wayland-scanner private-code \
  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
  xdg-shell.c

gcc -O3 -flto -march=native -fno-plt -c xdg-shell.c -o xdg-shell.o
g++ -O3 -flto -march=native -fno-plt -pthread wgts.cpp xdg-shell.o -o wayshaders -lwayland-client -lwayland-egl -lEGL -lGL