
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <switch.h>
#include "server.hpp"

#define SERVER_PORT 80
#define __DEBUG__ 1

nxgallery::WebServer* webServer;
Thread thread;
bool threadShouldRun = true;

void initSwitchModules()
{
    // Initialize the sockets service (needed for networking)
    Result r = socketInitializeDefault();
    if (R_FAILED(r))
        printf("ERROR initializing socket: %d\n", R_DESCRIPTION(r));

    // Initialize the romfs service (needed to serve the static web page from the file system)
    r = romfsInit();
    if (R_FAILED(r))
        printf("ERROR initializing romfs: %d\n", R_DESCRIPTION(r));

    // Initialize the filesystem service (needed to access the filesystem - obviously)
    r = fsInitialize();
    if (R_FAILED(r))
        printf("ERROR initializing fs: %d\n", R_DESCRIPTION(r));

    // Initialize the capsa service (needed to access the Switch's album)
    r = capsaInitialize();
    if (R_FAILED(r))
        printf("ERROR initializing capsa: %d\n", R_DESCRIPTION(r));

    // Initialize the setsys service (needed to get the Switch's color theme)
    r = setsysInitialize();
    if (R_FAILED(r))
        printf("ERROR initializing setsys: %d\n", R_DESCRIPTION(r));

    // Initialize the ns service (needed to translate title IDs to title names)
    r = nsInitialize();
    if (R_FAILED(r))
        printf("ERROR initializing ns: %d\n", R_DESCRIPTION(r));
}


void exitSwitchModules()
{
    nsExit();
    setsysExit();
    capsaExit();
    fsExit();
    romfsExit();
    socketExit();
}


void threadFunc(void* arg)
{
    printf("Thread started (webserver, serve loop)\n");
    while(1)
    {
        if(!threadShouldRun)
            break;
        webServer->ServeLoop();
        svcSleepThread(0000100000ull);
    }
}

void startWebBrowser(void* arg){
    Result rc;
    WebCommonConfig conf;
    WebCommonReply out;
    rc = webPageCreate(&conf, "http://127.0.0.1:80/");
    if (R_FAILED(rc))
        printf("Error starting Browser");
    webConfigSetJsExtension(&conf, true);
    webConfigSetPageCache(&conf, true);
    webConfigSetBootLoadingIcon(&conf, true);
    webConfigSetWhitelist(&conf, "^http*"); // .*
    rc = webConfigShow(&conf, &out);
    if (R_FAILED(rc))
        printf("Error starting Browser\n");
}


int main(int argc, char* argv[])
{
    consoleInit(NULL);
    initSwitchModules();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState pad;
    padInitializeDefault(&pad);

    webServer = new nxgallery::WebServer(SERVER_PORT);
    webServer->AddMountPoint("romfs:/www");
    webServer->Start();

   
    char serverAddress[32];
    webServer->GetAddress(serverAddress);
    printf("Open %s in your web browser\n", serverAddress);
    printf(CONSOLE_RESET);
    // webServer->AddMountPoint(albumPath);


    Result rc;
    // rc = threadCreate(&thread, threadFunc, NULL, NULL, 0x10000, 0x2C, -2);
    // rc = threadStart(&thread);
    rc = threadCreate(&thread, startWebBrowser, NULL, NULL, 0x10000, 0x2C, -2);
    // rc = threadStart(&thread);
    // if (R_SUCCEEDED(rc))
    // {
    //     rc = threadStart(&thread);
    //     if (R_SUCCEEDED(rc))
    //     {
    //         printf("Thread started\n");
    //     } else {
    //         printf("ERROR starting thread: %d\n", R_DESCRIPTION(rc));
    //     }
    // } else {
    //     printf("ERROR creating thread: %d\n", R_DESCRIPTION(rc));
    // }

    printf("Press A to open the webbrowser\n");

    while (appletMainLoop())
    {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus)
            break; // break in order to return to hbmenu

        if (kDown & HidNpadButton_A){
            threadCreate(&thread, startWebBrowser, NULL, NULL, 0x10000, 0x2C, -2);
            threadStart(&thread);
        }
            
            // startWebBrowser();

        webServer->ServeLoop();
        
        consoleUpdate(NULL);
    }

    threadShouldRun = false;
    threadWaitForExit(&thread);
    threadClose(&thread);

    webServer->Stop();
    exitSwitchModules();
    consoleExit(NULL);
    return 0;
}
