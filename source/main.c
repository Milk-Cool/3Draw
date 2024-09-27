#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>

bool last_pressed = false;

static SwkbdState kb_swkbd;
char* kb_input_alloc(const char* prompt, u32 size, bool predictive) {
    char* value = malloc(sizeof(char) * size);
    swkbdInit(&kb_swkbd, SWKBD_TYPE_NUMPAD, 2, -1);
    swkbdSetInitialText(&kb_swkbd, "");
    swkbdSetHintText(&kb_swkbd, prompt);
	swkbdSetNumpadKeys(&kb_swkbd, 0L, L'.');
    if(predictive) swkbdSetFeatures(&kb_swkbd, SWKBD_PREDICTIVE_INPUT);
    SwkbdButton btn = swkbdInputText(&kb_swkbd, value, size);
    return btn == SWKBD_BUTTON_CONFIRM ? value : NULL;
}
void kb_free(char* text) {
    free(text);
}

#define PACKET_SIZE 5
uint8_t buf[PACKET_SIZE];

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define DEPTH 3

#define TCP_BUF_LEN (4 * 1024 * 1024) // Max packet size

void* soc_sharedmem;

volatile bool run_screen = true;
int sock;

void thread_draw(void *arg) {
	(void)arg;
	size_t size = SCREEN_WIDTH * SCREEN_HEIGHT * DEPTH;
	uint8_t* img_full = malloc(size);
	uint8_t* img_part = malloc(1);
	int n = 0, m = 0, p = 0;
	// n is img_full index, m is img_part index (unused), p is parted packet size
	while(run_screen) {
		uint8_t* pack = malloc(TCP_BUF_LEN);
		int recd = read(sock, pack, TCP_BUF_LEN);
		pack = realloc(pack, recd);
		uint8_t* pack_orig = pack;
		while(recd > 0) {
			if(n > 0) {
				int avail = recd > p - n ? p - n : recd;
				memcpy(img_full + n, pack, avail);
				n += avail;
				pack += avail;
				recd -= avail;
				if(n == p) {
					n = 0;
					printf("copying screenshot\n");
					u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
					memcpy(fb, img_full, p);
				}
			} else if(recd >= 3) {
				uint32_t psize = ((uint32_t)pack[0] << 16) | ((uint32_t)pack[1] << 8) | pack[2];
				printf("d %d %u\n", recd - 3, psize);

				if(psize == 0) n = 0;
				else if(recd >= psize + 3) {
					memcpy(img_full, pack + 3, psize);
				} else {
					memcpy(img_full, pack + 3, recd - 3);
					p = psize;
					n = recd - 3;
				}
				recd -= psize + 3;
				pack += psize + 3;
			} else break;
		}
		free(pack_orig);
	}
	free(img_full);
	free(img_part);
}

int main(int argc, char **argv)
{
	gfxInitDefault();

	soc_sharedmem = (void*)memalign(0x1000, TCP_BUF_LEN);
    socInit((u32*)soc_sharedmem, TCP_BUF_LEN);

	//Initialize console on top screen. Using NULL as the second argument tells the console library to use the internal console structure as current one
	consoleInit(GFX_TOP, NULL);
	gfxSetDoubleBuffering(GFX_BOTTOM, false);

	printf("\x1b[0;0HPress Start to exit.\n");

	sock = socket(AF_INET, SOCK_STREAM, 0);

	char* text = kb_input_alloc("Enter the IP:", 16, false);
	printf("%s\n", text);
	struct sockaddr_in serv_addr;
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = inet_addr(text);
	serv_addr.sin_port = htons(6789);
	int n = connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
	printf("%d, %d\n", n, errno);
	int w, h;
	uint8_t wh[4];
	read(sock, wh, 4);
	w = (int)wh[0] << 8 | wh[1];
	h = (int)wh[2] << 8 | wh[3];
	free(text);

	int x = SCREEN_WIDTH / 2, y = SCREEN_HEIGHT / 2, sx = 0, sy = 0;

	Thread draw;
	s32 prio = 0;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	draw = threadCreate(thread_draw, NULL, 4 * 1024, prio - 1, -2, false);

	// Main loop
	while (aptMainLoop())
	{
		//Scan all the inputs. This should be done once for each frame
		hidScanInput();

		//hidKeysDown returns information about which buttons have been just pressed (and they weren't in the previous frame)
		u32 kHeld = hidKeysHeld();

		if (kHeld & KEY_START) break; // break in order to return to hbmenu

#define SEND0 \
	send(sock, buf, PACKET_SIZE, 0);
#define SEND \
	buf[1] = (sx >> 8) & 0xff; \
	buf[2] = (sx >> 0) & 0xff; \
	buf[3] = (sy >> 8) & 0xff; \
	buf[4] = (sy >> 0) & 0xff; \
	SEND0
#define SENDABS \
	buf[1] = (x >> 8) & 0xff; \
	buf[2] = (x >> 0) & 0xff; \
	buf[3] = (y >> 8) & 0xff; \
	buf[4] = (y >> 0) & 0xff; \
	SEND0
		if(kHeld & KEY_TOUCH) {
			touchPosition touch;

			//Read the touch screen coordinates
			hidTouchRead(&touch);

			//Print the touch screen coordinates
			buf[0] = 0x01;
			sx = touch.px, sy = touch.py;
			SEND
			last_pressed = true;
			printf("%x %d %d %d %d %d %d\n", buf[0], w, h, sx, sy, x, y);
		} else if(last_pressed) {
			buf[0] = 0x02;
			SEND
			last_pressed = false;
		} else if(kHeld & KEY_DLEFT
				|| kHeld & KEY_DRIGHT
				|| kHeld & KEY_DUP
				|| kHeld & KEY_DDOWN) {
			if(kHeld & KEY_DLEFT && x > SCREEN_WIDTH / 2) x--;
			if(kHeld & KEY_DRIGHT && x < w - SCREEN_WIDTH / 2 - 1) x++;
			if(kHeld & KEY_DUP && y > SCREEN_HEIGHT / 2) y--;
			if(kHeld & KEY_DDOWN && y < h - SCREEN_HEIGHT / 2 - 1) y++;
			buf[0] = 0x03;
			SENDABS
		} else if(kHeld & KEY_B) {
			buf[0] = 0x04;
			SEND0
		} else if(kHeld & KEY_X) {
			buf[0] = 0x05;
			SEND0
		} else {
			// We don't need this anymore
			// buf[0] = 0x00;
			// SEND
		}

		// Flush and swap framebuffers
		gfxFlushBuffers();
		gfxSwapBuffers();

		//Wait for VBlank
		gspWaitForVBlank();
	}

	run_screen = false;
	threadJoin(draw, U64_MAX);
	threadFree(draw);

	// Exit services
	gfxExit();
	return 0;
}
