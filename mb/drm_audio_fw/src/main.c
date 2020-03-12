/*
 * eCTF Collegiate 2020 MicroBlaze Example Code
 * Audio Digital Rights Management
 */

#include <stdio.h>
#include "platform.h"
#include "xparameters.h"
#include "xil_exception.h"
#include "xstatus.h"
#include "xaxidma.h"
#include "xil_mem.h"
#include "util.h"
#include "secrets.h"
#include "xintc.h"
#include "constants.h"
#include "sleep.h"
#include "wolfssl/wolfcrypt/hash.h"
#include "wolfssl/wolfcrypt/sha256.h"
#include "wolfssl/wolfcrypt/coding.h"
#include "wolfssl/wolfcrypt/wc_encrypt.h"
#include "wolfssl/wolfcrypt/hmac.h"


//////////////////////// GLOBALS ////////////////////////


// audio DMA access
static XAxiDma sAxiDma;

// LED colors and controller
u32 *led = (u32*) XPAR_RGB_PWM_0_PWM_AXI_BASEADDR;
const struct color RED =    {0x01ff, 0x0000, 0x0000};
const struct color YELLOW = {0x01ff, 0x01ff, 0x0000};
const struct color GREEN =  {0x0000, 0x01ff, 0x0000};
const struct color BLUE =   {0x0000, 0x0000, 0x01ff};

// change states
#define change_state(state, color) c->drm_state = state; setLED(led, color);
#define set_stopped() change_state(STOPPED, RED)
#define set_working() change_state(WORKING, YELLOW)
#define set_playing() change_state(PLAYING, GREEN)
#define set_paused()  change_state(PAUSED, BLUE)

// shared command channel -- read/write for both PS and PL
volatile cmd_channel *c = (cmd_channel*)SHARED_DDR_BASE;

// internal state store
internal_state s;


//////////////////////// INTERRUPT HANDLING ////////////////////////


// shared variable between main thread and interrupt processing thread
volatile static int InterruptProcessed = FALSE;
static XIntc InterruptController;

void myISR(void) {
    InterruptProcessed = TRUE;
}


//////////////////////// UTILITY FUNCTIONS ////////////////////////


// returns whether an rid has been provisioned
int is_provisioned_rid(char rid) {
    for (int i = 0; i < NUM_PROVISIONED_REGIONS; i++) {
        if (rid == PROVISIONED_RIDS[i]) {
            return TRUE;
        }
    }
    return FALSE;
}

// looks up the region name corresponding to the rid
int rid_to_region_name(char rid, char **region_name, int provisioned_only) {
    for (int i = 0; i < NUM_REGIONS; i++) {
        if (rid == REGION_IDS[i] &&
            (!provisioned_only || is_provisioned_rid(rid))) {
            *region_name = (char *)REGION_NAMES[i];
            return TRUE;
        }
    }

    mb_printf("Could not find region ID '%d'\r\n", rid);
    *region_name = "<unknown region>";
    return FALSE;
}


// looks up the rid corresponding to the region name
int region_name_to_rid(char *region_name, char *rid, int provisioned_only) {
    for (int i = 0; i < NUM_REGIONS; i++) {
        if (!strcmp(region_name, REGION_NAMES[i]) &&
            (!provisioned_only || is_provisioned_rid(REGION_IDS[i]))) {
            *rid = REGION_IDS[i];
            return TRUE;
        }
    }

    mb_printf("Could not find region name '%s'\r\n", region_name);
    *rid = -1;
    return FALSE;
}


// returns whether a uid has been provisioned
int is_provisioned_uid(char uid) {
    for (int i = 0; i < NUM_PROVISIONED_USERS; i++) {
        if (uid == PROVISIONED_UIDS[i]) {
            return TRUE;
        }
    }
    return FALSE;
}


// looks up the username corresponding to the uid
int uid_to_username(char uid, char **username, int provisioned_only) {
    for (int i = 0; i < NUM_USERS; i++) {
        if (uid == USER_IDS[i] &&
            (!provisioned_only || is_provisioned_uid(uid))) {
            *username = (char *)USERNAMES[i];
            return TRUE;
        }
    }

    mb_printf("Could not find uid '%d'\r\n", uid);
    *username = "<unknown user>";
    return FALSE;
}


// looks up the uid corresponding to the username
int username_to_uid(char *username, char *uid, int provisioned_only) {
    for (int i = 0; i < NUM_USERS; i++) {
        if (!strcmp(username, USERNAMES[USER_IDS[i]]) &&
            (!provisioned_only || is_provisioned_uid(USER_IDS[i]))) {
            *uid = USER_IDS[i];
            return TRUE;
        }
    }

    mb_printf("Could not find username '%s'\r\n", username);
    *uid = -1;
    return FALSE;
}


// loads the song metadata in the shared buffer into the local struct
void load_song_md() {
    s.song_md.md_size = c->song.md.md_size;
    s.song_md.owner_id = c->song.md.owner_id;
    s.song_md.num_regions = c->song.md.num_regions;
    s.song_md.num_users = c->song.md.num_users;
    memcpy(s.song_md.rids, (void *)get_drm_rids(c->song), s.song_md.num_regions);
    memcpy(s.song_md.uids, (void *)get_drm_uids(c->song), s.song_md.num_users);
    // TODO: CHECKSUM VERIFY FOR INTEGRITY
}


// checks if the song loaded into the shared buffer is locked for the current user
int is_locked() {
    int locked = TRUE;

    // check for authorized user
    if (!s.logged_in) {
        mb_printf("No user logged in");
    } else {
        load_song_md();

        // check if user is authorized to play song
        if (s.uid == s.song_md.owner_id) {
            locked = FALSE;
        } else {
            for (int i = 0; i < NUM_PROVISIONED_USERS && locked; i++) {
                if (s.uid == s.song_md.uids[i]) {
                    locked = FALSE;
                }
            }
        }

        if (locked) {
            mb_printf("User '%s' does not have access to this song", s.username);
            return locked;
        }
        mb_printf("User '%s' has access to this song", s.username);
        locked = TRUE; // reset lock for region check

        // search for region match
        for (int i = 0; i < s.song_md.num_regions; i++) {
            for (int j = 0; j < (u8)NUM_PROVISIONED_REGIONS; j++) {
                if (PROVISIONED_RIDS[j] == s.song_md.rids[i]) {
                    locked = FALSE;
                }
            }
        }

        if (!locked) {
            mb_printf("Region Match. Full Song can be played. Unlocking...");
        } else {
            mb_printf("Invalid region");
        }
    }
    return locked;
}


// copy the local song metadata into buf in the correct format
// returns the size of the metadata in buf (including the metadata size field)
// song metadata should be loaded before call
int gen_song_md(char *buf) {
    buf[0] = ((5 + s.song_md.num_regions + s.song_md.num_users) / 2) * 2; // account for parity
    buf[1] = s.song_md.owner_id;
    buf[2] = s.song_md.num_regions;
    buf[3] = s.song_md.num_users;
    memcpy(buf + 4, s.song_md.rids, s.song_md.num_regions);
    memcpy(buf + 4 + s.song_md.num_regions, s.song_md.uids, s.song_md.num_users);

    return buf[0];
}


// takes the base64 encoded cryptographic keys from the secrets header and
// decodes them for use
// copy decoded keys into local internal state
// return 0 on success
int initCryptoKeys() {
    word32 outLen = b64AES_KEY_SZ;
    if (Base64_Decode((void *)AES_KEY, (word32)b64AES_KEY_SZ, (void *)s.aesKey, &outLen) != 0) {
        return -1;
    }
    outLen = b64AES_KEY_SZ;
    if (Base64_Decode((void *)HMAC_MD_KEY, (word32)b64HMAC_MD_KEY_SZ, (void *)s.hmacMdKey, &outLen) != 0) {
        return -1;
    }
    outLen = b64AES_KEY_SZ;
    if (Base64_Decode((void *)HMAC_KEY, (word32)b64HMAC_KEY_SZ, (void *)s.hmacKey, &outLen) != 0) {
        return -1;
    }
    return 0;
}

/* create HMAC using data and key and compare to DRM HMAC
 * return 0 on success, -1 otherwise
 *
 * hmac    : Hmac object
 * data    : data to create HMAC with
 * dataLen : length of data in bytes
 * hash    : buffer to store hash of data
 * drmHmac : HMAC to compare computed HMAC to
 */
int verifyHmac(Hmac *hmac, char* data, int dataLen, char* hash, char* drmHmac) {
    if (wc_HmacUpdate(hmac, (void *)data, dataLen) != 0) {
        return -1;
    }
    if (wc_HmacFinal(hmac, hash) != 0) {
        return -1;
    }
    if (memcmp(hash, (void *)drmHmac, SIGNATURE_SZ) != 0) {
        return -1;
    }
    return 0;
}


//////////////////////// COMMAND FUNCTIONS ////////////////////////


// attempt to log in to the credentials in the shared buffer
void login() {
    // first, copy attempted username and pin into local internal_state
    memcpy((void*)s.username, (void*)c->username, USERNAME_SZ);
    memcpy((void*)s.pin, (void*)c->pin, MAX_PIN_SZ);

    // clear shared memory
    memset((void*)c->username, 0, USERNAME_SZ);
    memset((void*)c->pin, 0, MAX_PIN_SZ);

    if (s.logged_in) {
        mb_printf("Already logged in. Please log out first.\r\n");
    } else {
        for (int i = 0; i < NUM_PROVISIONED_USERS; i++) {
            // search for matching username
            if (!strcmp((void*)s.username, USERNAMES[PROVISIONED_UIDS[i]])) {
                // check if pin matches
                if (!strcmp((void*)s.pin, PROVISIONED_PINS[i])) {
                    //update state
                    s.logged_in = 1;
                    s.uid = PROVISIONED_UIDS[i];
                    mb_printf("Logged in for user '%s'\r\n", (void *)s.username);
                    return;
                } else {
                    break;
                }
            }
        }
        // reject login attempt and wait 5 seconds
        mb_printf("Login unsuccessful\r\n");
        usleep(5000000); // TODO: actually must be (5 - max time in loop above) seconds
    }
}


// attempt to log out
void logout() {
    if (c->login_status) {
        mb_printf("Logging out...\r\n");
        s.logged_in = 0;
        c->login_status = 0;
        memset((void*)c->username, 0, USERNAME_SZ);
        memset((void*)c->pin, 0, MAX_PIN_SZ);
        s.uid = 0;
    } else {
        mb_printf("Not logged in\r\n");
    }
}


// handles a request to query the player's metadata
void query_player() {
    c->query.num_regions = NUM_PROVISIONED_REGIONS;
    c->query.num_users = NUM_PROVISIONED_USERS;

    for (int i = 0; i < NUM_PROVISIONED_REGIONS; i++) {
        strcpy((char *)q_region_lookup(c->query, i), REGION_NAMES[PROVISIONED_RIDS[i]]);
    }

    for (int i = 0; i < NUM_PROVISIONED_USERS; i++) {
        strcpy((char *)q_user_lookup(c->query, i), USERNAMES[i]);
    }

    mb_printf("Queried player (%d regions, %d users)\r\n", c->query.num_regions, c->query.num_users);
}


// handles a request to query song metadata
void query_song() {
    char *name;

    // load song
    load_song_md();
    memset((void *)&c->query, 0, sizeof(query));

    c->query.num_regions = s.song_md.num_regions;
    c->query.num_users = s.song_md.num_users;

    // copy owner name
    uid_to_username(s.song_md.owner_id, &name, FALSE);
    strcpy((char *)c->query.owner, name);

    // copy region names
    for (int i = 0; i < s.song_md.num_regions; i++) {
        rid_to_region_name(s.song_md.rids[i], &name, FALSE);
        strcpy((char *)q_region_lookup(c->query, i), name);
    }

    // copy authorized uid names
    for (int i = 0; i < s.song_md.num_users; i++) {
        uid_to_username(s.song_md.uids[i], &name, FALSE);
        strcpy((char *)q_user_lookup(c->query, i), name);
    }

    mb_printf("Queried song (%d regions, %d users)\r\n", c->query.num_regions, c->query.num_users);
}


// add a user to the song's list of users
void share_song() {
    int new_md_len, shift;
    char new_md[256], uid;

    // reject non-owner attempts to share
    load_song_md();
    if (!s.logged_in) {
        mb_printf("No user is logged in. Cannot share song\r\n");
        c->song.wav_size = 0;
        return;
    } else if (s.uid != s.song_md.owner_id) {
        mb_printf("User '%s' is not song's owner. Cannot share song\r\n", s.username);
        c->song.wav_size = 0;
        return;
    } else if (!username_to_uid((char *)c->username, &uid, TRUE)) {
        mb_printf("Username not found\r\n");
        c->song.wav_size = 0;
        return;
    } else if (s.song_md.num_users >= MAX_USERS) {
        mb_printf("Cannot share song\r\n");
        c->song.wav_size = 0;
        return;
    }
    
    // generate new song metadata
    s.song_md.uids[s.song_md.num_users++] = uid;
    new_md_len = gen_song_md(new_md);
    shift = new_md_len - s.song_md.md_size;

    // shift over song and add new metadata
    if (shift) {
        memmove((void *)get_drm_song(c->song) + shift, (void *)get_drm_song(c->song), c->song.wav_size);
    }
    memcpy((void *)&c->song.md, new_md, new_md_len);

    // update file size
    c->song.file_size += shift;
    c->song.wav_size  += shift;

    mb_printf("Shared song with '%s'\r\n", c->username);
}


// plays a song and looks for play-time commands
void play_song() {
    u32 counter = 0, rem, cp_num, cp_xfil_cnt, offset, dma_cnt, lenAudio, *fifo_fill, length;
    int ret = -1;
    char hash[SIGNATURE_SZ];
    Hmac hmac;

    // use HMAC metadata key for verifying metadata (and rest of data)
    if (wc_HmacSetKey(&hmac, SHA256, s.hmacMdKey, HMAC_MD_KEY_SZ) != 0) {
        mb_printf("Operation failed REMOVE->HMACSETKEY");
        return;
    }

    mb_printf("Reading Audio File...");
    load_song_md();

    // WAV size is size of all data following the WAV metadata
    length = c->song.wav_size - SIGNATURE_SZ;
    lenAudio = c->song.wav_size - SIGNATURE_SZ - c->song.md.md_size - AES_BLK_SZ;

    mb_printf("Verifying Audio File...");
    if (verifyHmac(&hmac, /*drm except wav md and hmac*/, /*datalen*/, hash, /*get_drm_hmac*/) != 0) {
        mb_printf("Failed to play audio");
        return;
    }
    mb_printf("Successfully Verified Audio File");

    // calculate total number of chunks to decrypt
    mb_printf("Song length = %dB", lenAudio);
    int nchunks = ((lenAudio % CHUNK_SZ) == 0) ? (lenAudio / CHUNK_SZ) : ((lenAudio / CHUNK_SZ)+1);
    mb_printf("# chunks: %d", nchunks);

    // truncate song if locked
    if (lenAudio > PREVIEW_SZ && is_locked()) {
        lenAudio = PREVIEW_SZ;
        mb_printf("Song is locked.  Playing only %ds = %dB\r\n",
                   PREVIEW_TIME_SEC, PREVIEW_SZ);
    } else {
        mb_printf("Song is unlocked. Playing full song\r\n");
    }

    int firstChunk = TRUE; // whether we are operating on the first chunk of the audio
    char iv[AES_BLK_SZ]; // use this as iv for all audio chunks after the first
    // stack size MUST be increased to fit this (default is 1KB)
    char plainChunk[CHUNK_SZ]; // current decrypted chunk

    // use HMAC key for verifying audio chunks
    if (wc_HmacSetKey(&hmac, SHA256, s.hmacKey, HMAC_KEY_SZ) != 0) {
        mb_printf("Operation failed REMOVE->HMACSETKEY");
        return;
    }

    rem = lenAudio;
    fifo_fill = (u32 *)XPAR_FIFO_COUNT_AXI_GPIO_0_BASEADDR;

    // write entire file to two-block codec fifo
    // writes to one block while the other is being played
    set_playing();
    while(rem > 0) {
        // check for interrupt to stop playback
        while (InterruptProcessed) {
            InterruptProcessed = FALSE;

            switch (c->cmd) {
            case PAUSE:
                mb_printf("Pausing... \r\n");
                set_paused();
                while (!InterruptProcessed) continue; // wait for interrupt
                break;
            case PLAY:
                mb_printf("Resuming... \r\n");
                set_playing();
                break;
            case STOP:
                mb_printf("Stopping playback...");
                return;
            case RESTART:
                mb_printf("Restarting song... \r\n");
                rem = lenAudio; // reset song counter
                firstChunk = TRUE;
                set_playing();
            default:
                break;
            }
        }

        // calculate write size and offset
        cp_num = (rem > CHUNK_SZ) ? CHUNK_SZ : rem;
        offset = (counter++ % 2 == 0) ? 0 : CHUNK_SZ;

        // if first chunk, grab the IV for decryption
        // if not the first chunk, use the most previous block as te IV
        if (firstChunk) {
            firstChunk = FALSE;
            memcpy(iv, (void *)(get_drm_aesiv(c->song)), AES_BLK_SZ);
        } else {
            memcpy(iv, (void *)(get_drm_song(c->song) + lenAudio - rem - AES_BLK_SZ), AES_BLK_SZ);
        }

        // verify chunk using HMAC
        if (verifyHmac(&hmac, (char*)(get_drm_song(c->song) + lenAudio - rem), cp_num, hash, /*get_drm_hmac*/) != 0) {
            mb_printf("Failed to play audio");
            return;
        }

        // decrypt chunk
        ret = wc_AesCbcDecryptWithKey((byte*)plainChunk, (void*)(get_drm_song(c->song) + lenAudio - rem), cp_num, (byte*)s.aesKey, (word32)AES_KEY_SZ, (byte*)iv);
        if (ret != 0) {
            mb_printf("Operation failed REMOVE DECRYPT");
            return;
        }

        // if last chunk unpad using PKCS#7
        if (counter == nchunks) {
            // TODO
            //mb_printf("last block: %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x", (void*)plainChunk[7200], (void*)plainChunk[7201], (void*)plainChunk[7202], (void*)plainChunk[7203], (void*)plainChunk[7204], (void*)plainChunk[7205], (void*)plainChunk[7206], (void*)plainChunk[7207], (void*)plainChunk[7208], (void*)plainChunk[7209], (void*)plainChunk[7210], (void*)plainChunk[7211], (void*)plainChunk[7212], (void*)plainChunk[7213], (void*)plainChunk[7214], (void*)plainChunk[7215]);
        }

        // do first mem cpy here into DMA BRAM
        Xil_MemCpy((void *)(XPAR_MB_DMA_AXI_BRAM_CTRL_0_S_AXI_BASEADDR + offset),
                   (void*)plainChunk,
                   (u32)(cp_num));
        
        cp_xfil_cnt = cp_num;

        while (cp_xfil_cnt > 0) {
            // polling while loop to wait for DMA to be ready
            // DMA must run first for this to yield the proper state
            // rem != lenAudio checks for first run
            while (XAxiDma_Busy(&sAxiDma, XAXIDMA_DMA_TO_DEVICE)
                   && rem != lenAudio && *fifo_fill < (FIFO_CAP - 32));

            // do DMA
            dma_cnt = (FIFO_CAP - *fifo_fill > cp_xfil_cnt)
                      ? FIFO_CAP - *fifo_fill
                      : cp_xfil_cnt;
            fnAudioPlay(sAxiDma, offset, dma_cnt);
            cp_xfil_cnt -= dma_cnt;
        }

        rem -= cp_num;
    }
}


// removes DRM data from song for digital out
void digital_out() {
    // remove metadata size from file and chunk sizes
    c->song.file_size -= c->song.md.md_size;
    c->song.wav_size -= c->song.md.md_size;

    if (is_locked() && PREVIEW_SZ < c->song.wav_size) {
        mb_printf("Only playing 30 seconds");
        c->song.file_size -= c->song.wav_size - PREVIEW_SZ;
        c->song.wav_size = PREVIEW_SZ;
    }

    // move WAV file up in buffer, skipping metadata
    mb_printf(MB_PROMPT "Dumping song (%dB)...", c->song.wav_size);
    memmove((void *)&c->song.md, (void *)get_drm_song(c->song), c->song.wav_size);

    mb_printf("Song dump finished\r\n");
}


//////////////////////// MAIN ////////////////////////


int main() {
    u32 status;

    init_platform();
    microblaze_register_handler((XInterruptHandler)myISR, (void *)0);
    microblaze_enable_interrupts();

    // Initialize the interrupt controller driver so that it is ready to use.
    status = XIntc_Initialize(&InterruptController, XPAR_INTC_0_DEVICE_ID);
    if (status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    // Set up the Interrupt System.
    status = SetUpInterruptSystem(&InterruptController, (XInterruptHandler)myISR);
    if (status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    // Congigure the DMA
    status = fnConfigDma(&sAxiDma);
    if(status != XST_SUCCESS) {
        mb_printf("DMA configuration ERROR\r\n");
        return XST_FAILURE;
    }

    // Start the LED
    enableLED(led);
    set_stopped();

    // clear command channel
    memset((void*)c, 0, sizeof(cmd_channel));

    mb_printf("Audio DRM Module has Booted\n\r");

    // WolfCrypt init
    if (wolfCrypt_Init() != 0) {
        mb_printf("Error in wolfCrypt_Init\r\n");
        return XST_FAILURE;
    }

    // initialize crypto keys
    if (initCryptoKeys() != 0) {
        mb_printf("Error initializing keys\r\n");
        return XST_FAILURE;
    }

    // Handle commands forever
    while(1) {
        // wait for interrupt to start
        if (InterruptProcessed) {
            InterruptProcessed = FALSE;
            set_working();

            // c->cmd is set by the miPod player
            switch (c->cmd) {
            case LOGIN:
                login();
                break;
            case LOGOUT:
                logout();
                break;
            case QUERY_PLAYER:
                query_player();
                break;
            case QUERY_SONG:
                query_song();
                break;
            case SHARE:
                share_song();
                break;
            case PLAY:
                play_song();
                mb_printf("Done Playing Song\r\n");
                break;
            case DIGITAL_OUT:
                digital_out();
                break;
            default:
                break;
            }

            // reset statuses and sleep to allowe player to recognize WORKING state
            strcpy((char *)c->username, s.username);
            c->login_status = s.logged_in;
            usleep(500);
            set_stopped();
        }
    }

    // WolfCrypt cleanup */
    if (wolfCrypt_Cleanup() != 0) {
        mb_printf("Error in wolfCrypt_Cleanup\r\n");
        return XST_FAILURE;
    }
    cleanup_platform();
    return 0;
}
