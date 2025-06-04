LDFLAGS+=-L/usr/local/lib -lnotcurses -lnotcurses-core -lavutil -lavformat -lavcodec -lswresample -lavdevice -lswscale 
#
CFLAGS+=-L/usr/local/lib
LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:/usr/local/lib
