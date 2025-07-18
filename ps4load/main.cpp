#include <unistd.h>
#include <zlib.h>
#include <arpa/inet.h>
#include <orbis/NetCtl.h>
#include <orbis/SystemService.h>
#include <dbglogger.h>
#include <zip.h>


#include <sstream>
#include <iostream>
#include <thread>
#include <mutex>
#include <orbis/libkernel.h>

#include "_common/graphics.h"
#include "_common/log.h"

// Dimensions
#define FRAME_WIDTH     1920
#define FRAME_HEIGHT    1080
#define FRAME_DEPTH        4

// Font information
#define FONT_SIZE   	   42
#define NOTIFICATION_ICON_DEFAULT       "cxml://psnotification/tex_default_icon_notification"

#define MIN(a, b)               ((a) < (b) ? (a) : (b))
#define MAX_ARG_COUNT           0x100
#define SELF_PATH               "/data/ps4load.tmp"
#define PORT                    4299
#define CHUNK                   0x4000
#define PKZIP                   0x04034B50
// Logging
std::stringstream debugLogStream;

// Background and foreground colors
Color bgColor;
Color fgColor;
Color errColor;

// Font faces
FT_Face fontTxt;

int frameID = 0;

#define ERROR(a, msg) { \
    if (a < 0) { \
        snprintf(msg_error, sizeof(msg_error), "PS4Load: " msg ); \
        usleep(250); \
        if(my_socket >= 0) { close(my_socket);my_socket = -1;} \
    } \
}
#define ERROR2(a, msg) { \
    if (a < 0) { \
        snprintf(msg_error, sizeof(msg_error), "PS4Load: %s", msg ); \
        usleep(1000000); \
        msg_error[0] = 0; \
        usleep(60); \
        goto reloop; \
    } \
}
#define continueloop() { close(c); goto reloop; }

char msg_error[128];
char msg_two  [128];

volatile int my_socket=-1;
volatile int flag_exit=0;

void notify_popup(const char *p_Uri, const char *p_Format, ...)
{
    OrbisNotificationRequest s_Request;
    memset(&s_Request, '\0', sizeof(s_Request));

    s_Request.reqId = NotificationRequest;
    s_Request.unk3 = 0;
    s_Request.useIconImageUri = 0;
    s_Request.targetId = -1;

    // Maximum size to move is destination size - 1 to allow for null terminator
    if (p_Uri)
    {
        s_Request.useIconImageUri = 1;
        strlcpy(s_Request.iconUri, p_Uri, sizeof(s_Request.iconUri));
    }

    va_list p_Args;
    va_start(p_Args, p_Format);
    vsnprintf(s_Request.message, sizeof(s_Request.message), p_Format, p_Args);
    va_end(p_Args);

    sceKernelSendNotificationRequest(NotificationRequest, &s_Request, sizeof(s_Request), 0);
    sceKernelUsleep(1000);
}

/* Decompress from source data to file dest until stream ends or EOF.
   inf() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_DATA_ERROR if the deflate data is
   invalid or incomplete, Z_VERSION_ERROR if the version of zlib.h and
   the version of the library linked do not match, or Z_ERRNO if there
   is an error reading or writing the files. */
int inflate_data(int source, uint32_t filesize, FILE *dest)
{
    int ret;
    uint32_t have;
    z_stream strm;
    uint8_t src[CHUNK];
    uint8_t out[CHUNK];

    /* allocate inflate state */
    memset(&strm, 0, sizeof(z_stream));
    ret = inflateInit(&strm);
    if (ret != Z_OK)
        return ret;

    /* decompress until deflate stream ends or end of file */
    do {
        strm.avail_in = MIN(CHUNK, filesize);
        ret = read(source, src, strm.avail_in);
        if (ret < 0) {
            (void)inflateEnd(&strm);
            return Z_ERRNO;
        }

        strm.avail_in = ret;
        if (strm.avail_in == 0)
            break;
        strm.next_in = src;
        filesize -= ret;

        /* run inflate() on input until output buffer not full */
        do {
            strm.avail_out = CHUNK;
            strm.next_out = out;
            ret = inflate(&strm, Z_NO_FLUSH);
            switch (ret) {
            case Z_STREAM_ERROR:
            case Z_NEED_DICT:
                ret = Z_DATA_ERROR;     /* and fall through */
            case Z_DATA_ERROR:
            case Z_MEM_ERROR:
                (void)inflateEnd(&strm);
                return ret;
            }
            have = CHUNK - strm.avail_out;
            if (fwrite(out, 1, have, dest) != have || ferror(dest)) {
                (void)inflateEnd(&strm);
                return Z_ERRNO;
            }
        } while (strm.avail_out == 0);

        /* done when inflate() says it's done */
    } while (ret != Z_STREAM_END && filesize);

    /* clean up and return */
    (void)inflateEnd(&strm);
    return (ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR);
}

int dump_data(int source, uint32_t filesize, FILE *dest)
{
    uint8_t data[CHUNK];
    uint32_t count, pkz = PKZIP;

    while (filesize > 0)
    {
        count = MIN(CHUNK, filesize);
        int ret = read(source, data, count);
        if (ret < 0)
            return Z_DATA_ERROR;

        if (pkz == PKZIP)
            pkz = memcmp(data, &pkz, 4);

        fwrite(data, ret, 1, dest);
        filesize -= ret;
    }

    return (pkz ? Z_OK : PKZIP);
}

void launch_self(const char* path, const char** args)
{
    dbglogger_log(msg_two);
    dbglogger_stop();
    sleep(1);

    sceSystemServiceLoadExec(path, args);
    sceKernelUsleep(2 * 1000000);
}

int netThread(void* data)
{
    if (access("/data/ps4load/eboot.bin", F_OK) == Z_OK)
    {
        snprintf(msg_two, sizeof(msg_two), "Loading eboot.bin...");
        launch_self("/data/ps4load/eboot.bin", NULL);
    }

    OrbisNetCtlInfo ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &ip_info);

    snprintf(msg_two, sizeof(msg_two), "Creating socket...");
    dbglogger_log(msg_two);

    my_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    ERROR(my_socket, "Error creating socket()");

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(PORT);
    
    if(my_socket!=-1) {
        snprintf(msg_two, sizeof(msg_two), "Binding socket...");
        dbglogger_log(msg_two);

        ERROR(bind(my_socket, (struct sockaddr*)&server, sizeof(server)), "Error bind()ing socket");
    }

    if(my_socket!=-1)
        ERROR(listen(my_socket, 10), "Error calling listen()");

reloop:
    msg_two[0] = 0;

    while (1) {
        
        usleep(20000);
        
        if(flag_exit) break;
        if(my_socket == -1) continue;

        //fgColor.r = fgColor.g = fgColor.b = 0xff;
        snprintf(msg_two, sizeof(msg_two), "%s:%d ready for client...", ip_info.ip_address, PORT);
        dbglogger_log(msg_two);

        int c = accept(my_socket, NULL, NULL);

        if(flag_exit) break;
        if(my_socket == -1) continue;

        ERROR(c, "Error calling accept()");

        uint32_t magic = 0;
        if (read(c, &magic, sizeof(magic)) < 0)
            continueloop();
        if (strncmp((char*)&magic, "HAXX", 4)) {
            snprintf(msg_two, sizeof(msg_two), "Wrong HAXX magic.");
            dbglogger_log(msg_two);
            continueloop();
        }
        if (read(c, &magic, sizeof(magic)) < 0)
            continueloop();
        uint16_t argslen = __bswap32(magic) & 0x0000FFFF;
        
        uint32_t filesize = 0;
        if (read(c, &filesize, sizeof(filesize)) < 0)
            continueloop();

        uint32_t uncompressed = 0;
        if (read(c, &uncompressed, sizeof(uncompressed)) < 0)
            continueloop();

        filesize = __bswap32(filesize);
        uncompressed = __bswap32(uncompressed);

        remove(SELF_PATH);
        FILE *fd = fopen(SELF_PATH, "wb");
        if (!fd) {
            close(c);
            ERROR2(-1, "Error opening temporary file.");
        }

        //fgColor.g = 255;
        snprintf(msg_two, sizeof(msg_two), "Receiving data... (0x%08x/0x%08x)", filesize, uncompressed);
        dbglogger_log(msg_two);

        int ret = uncompressed ? inflate_data(c, filesize, fd) : dump_data(c, filesize, fd);
        fclose(fd);

        if (ret != Z_OK && ret != PKZIP)
            continueloop();

        snprintf(msg_two, sizeof(msg_two), "Receiving arguments... 0x%08x", argslen);
        dbglogger_log(msg_two);

        uint8_t* args = NULL;
        if (argslen) {
            args = (uint8_t*)malloc(argslen);
            if (read(c, args, argslen) < 0)
                continueloop();
        }
        close(c);

        if (!uncompressed && ret == PKZIP)
        {
            snprintf(msg_two, sizeof(msg_two), "Extracting ZIP to /data/ ...");
            dbglogger_log(msg_two);

            ERROR2(zip_extract(SELF_PATH, "/data/", NULL, NULL), "Error extracting ZIP file.");
            goto reloop;
        }

        ERROR2(chmod(SELF_PATH, 0777), "Failed to chmod() temporary file.");

        char* launchargv[MAX_ARG_COUNT];
        memset(launchargv, 0, sizeof(launchargv));

        int i = 0, pos = 0;
        while (pos < argslen) {
            int len = strlen((char*)args + pos);
            if (!len)
                break;
            launchargv[i] = (char*)malloc(len + 1);
            strcpy(launchargv[i], (char*)args + pos);
            pos += len + 1;
            i++;
        }

        snprintf(msg_two, sizeof(msg_two), "Launching...");
        launch_self(SELF_PATH, (const char**)launchargv);
    }

    return 0;
}

int main()
{
    int rc;
    int video;
    int curFrame = 0;
    
    // No buffering
    setvbuf(stdout, NULL, _IONBF, 0);
    
    // Create a 2D scene
    DEBUGLOG << "Creating a scene";
    
    auto scene = new Scene2D(FRAME_WIDTH, FRAME_HEIGHT, FRAME_DEPTH);
    
    if(!scene->Init(0xC000000, 2))
    {
        notify_popup(NOTIFICATION_ICON_DEFAULT, "Failed to initialize 2D scene%s", "");
    	DEBUGLOG << "Failed to initialize 2D scene";
    	for(;;);
    }

    // Set colors
    bgColor = { 0, 0, 0 };
    fgColor = { 255, 255, 255 };
    errColor = { 255, 0, 0 };

    // Initialize the font faces with arial (must be included in the package root!)
    const char *font = "/app0/assets/fonts/Gontserrat-Regular.ttf";
    
    DEBUGLOG << "Initializing font (" << font << ")";

    if(!scene->InitFont(&fontTxt, font, FONT_SIZE))
    {
        notify_popup(NOTIFICATION_ICON_DEFAULT, "Failed to initialize font%s", "");
    	DEBUGLOG << "Failed to initialize font '" << font << "'";
    	for(;;);
    }

    DEBUGLOG << "Entering draw loop...";

    bool done = false;
    // Setup thread
    std::thread t1([](void* data) {
    netThread(data);
}, static_cast<void*>(&done));
    t1.detach();

    // Draw loop
    notify_popup(NOTIFICATION_ICON_DEFAULT, "Start%s", "");
    while(!done)
    {
        scene->DrawText((char *)(std::string("PS4Load\n") + msg_two).c_str(), fontTxt, 150, 150, bgColor, fgColor);
        if (msg_error[0]) {
            scene->DrawText((char *)(std::string("Error:\n") + msg_error).c_str(), fontTxt, 150, 300, bgColor, errColor);
        }

        // Submit the frame buffer
        scene->SubmitFlip(frameID);
        scene->FrameWait(frameID);

        // Swap to the next buffer
        scene->FrameBufferSwap();
        frameID++;
    }

    return 0;
}

