/* What the in-game overlay shows about online play (runtime_online.cpp), without the network headers. */
#ifndef DCP_RUNTIME_ONLINE_OVERLAY_H
#define DCP_RUNTIME_ONLINE_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when Online Play is on (dcp.ini or DCP_ONLINE), even if this build or identity then keeps the game offline. */
int runtime_online_play_enabled(void);
/* The worst P2P round trip in ms among the game's connected peers; -1 when offline or no peer has measured one. */
int runtime_online_peer_ping_ms(void);
/* Copies the next online notice for the player into text (NUL-terminated) and returns 1; 0 when there is none. */
int runtime_online_take_notice(char *text, int size);

#define RUNTIME_ONLINE_MAX_FRIENDS 128
#define RUNTIME_ONLINE_MAX_TEST_LINES 8

enum { RUNTIME_ONLINE_LINK_OFF, RUNTIME_ONLINE_LINK_CONNECTING, RUNTIME_ONLINE_LINK_UP, RUNTIME_ONLINE_LINK_UNREACHABLE };
enum { RUNTIME_ONLINE_TEST_IDLE, RUNTIME_ONLINE_TEST_RUNNING, RUNTIME_ONLINE_TEST_DONE };
enum { RUNTIME_ONLINE_MARK_OK, RUNTIME_ONLINE_MARK_NOTE, RUNTIME_ONLINE_MARK_PROBLEM };

typedef struct {
    char name[24];
    char status[80]; /* "Online, in a room · Windows · v0.8.0", as the launcher words it */
    int online;
    int incompatible;
} RuntimeOnlineFriend;

typedef struct {
    int mark;
    char label[32];
    char text[160];
} RuntimeOnlineTestLine;

/* A copy of what the Online page shows, taken under the runtime's locks; the overlay holds no network state. */
typedef struct {
    int active; /* the game goes online: a supported build, its imports and an identity */
    int link;   /* RUNTIME_ONLINE_LINK_* of the game's lobby connection */
    char name[24];
    char friend_code[16]; /* empty until the lobby has answered */
    int friends_known;    /* the lobby has sent the friend list at least once */
    int friend_total;     /* may exceed RUNTIME_ONLINE_MAX_FRIENDS */
    int friend_count;     /* entries filled in `friends` */
    RuntimeOnlineFriend friends[RUNTIME_ONLINE_MAX_FRIENDS];
    int test;             /* RUNTIME_ONLINE_TEST_* */
    int test_ready;
    char test_verdict[96];
    int test_line_count;
    RuntimeOnlineTestLine test_lines[RUNTIME_ONLINE_MAX_TEST_LINES];
} RuntimeOnlinePage;

/* Copies what the game's own Online runtime already knows into `page`. It only reads: it never connects, retries, sends or
 * refreshes anything, and never waits on the network. */
void runtime_online_page_update(RuntimeOnlinePage *page);
/* Starts the launcher's Connection test on a worker thread; ignored while one runs. */
void runtime_online_page_run_test(void);

#ifdef __cplusplus
}
#endif

#endif
