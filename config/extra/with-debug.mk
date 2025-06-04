CPPFLAGS+=-g -Og -ggdb3
CFLAGS+=-Og -DFD_DEBUG_MODE
CPPFLAGS+=-fno-omit-frame-pointer
LDFLAGS+=-rdynamic -fuse-ld=mold
