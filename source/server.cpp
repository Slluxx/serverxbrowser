/*
    NXGallery for Nintendo Switch
    Made with love by Jonathan Verbeek (jverbeek.de)
*/
#include <stdarg.h>
#include <inttypes.h>
#include "server.hpp"

#define __DEBUG__

using namespace nxgallery;


WebServer::WebServer(int port)
{
    // Store the port
    this->port = port;

    // We're not running yet
    isRunning = false;

    // We won't initialize the web server here just now, 
    // the caller can do that by calling WebServer::Start
}

void WebServer::Start()
{
    // If we're already running, don't try to start again
    if (isRunning)
        return;

    // Construct a socket address where we want to listen for requests
    static struct sockaddr_in serv_addr;
    serv_addr.sin_addr.s_addr = INADDR_ANY; // The Switch'es IP address
    serv_addr.sin_port = htons(port);
    serv_addr.sin_family = AF_INET; // The Switch only supports AF_INET and AF_ROUTE: https://switchbrew.org/wiki/Sockets_services#Socket

    // Create a new STREAM IPv4 socket
    serverSocket = socket(PF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0)
    {
        printf("Failed to create a web server socket: %d\n", errno);
        return;
    }

    // Set a relatively short timeout for recv() calls, see WebServer::ServeRequest for more info why
    struct timeval recvTimeout;
    recvTimeout.tv_sec = 1;
    recvTimeout.tv_usec = 0;
    setsockopt(serverSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&recvTimeout, sizeof(recvTimeout));

    // Enable address and port reusing
    int yes = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

    // Bind the just-created socket to the address
    if (bind(serverSocket, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        printf("Failed to bind web server socket: %d\n", errno);
        return;
    }

    // Start listening to the socket with 5 maximum parallel connections
    if (listen(serverSocket, 10) < 0)
    {
        printf("Failed to listen to the web server socket: %d\n", errno);
        return;
    }

    // Now we're running
    isRunning = true;
}

void WebServer::GetAddress(char* buffer)
{
    static struct sockaddr_in serv_addr;
    serv_addr.sin_addr.s_addr = gethostid();
    sprintf(buffer, "%s:%d", inet_ntoa(serv_addr.sin_addr), port);
}

void WebServer::AddMountPoint(const char* path)
{
    // Add it to the mountPoints vector
    mountPoints.push_back(path);
}

void WebServer::ServeLoop()
{
    // Asynchronous / event-driven loop using poll
    // More here: http://man7.org/linux/man-pages/man2/poll.2.html

    // Do not try to serve anything when the server isn't running
    if (!isRunning)
        return;

    // Will hold the data returned from poll()
    struct pollfd pollInfo;
    pollInfo.fd = serverSocket; // Listen to events from the server socket we opened
    pollInfo.events = POLLIN; // Only react on incoming events
    pollInfo.revents = 0; // Gets filled with events later

    // Poll for new events
    if (poll(&pollInfo, 1, 0) > 0)
    {
        printf("poll1\n");
        // There was an incoming event on the server socket
        if (pollInfo.revents & POLLIN)
        {
            printf("poll2\n");
            // Will hold data about the new connection
            struct sockaddr_in clientAddress;
            socklen_t addrLen = sizeof(clientAddress);

            // Accept the incoming connection
            int acceptedConnection = accept(serverSocket, (struct sockaddr*)&clientAddress, &addrLen);
            if (acceptedConnection > 0)
            {
#ifdef __DEBUG__
                printf("Accepted connection from %s:%u\n", inet_ntoa(clientAddress.sin_addr), ntohs(clientAddress.sin_port));
#endif

                // Serve ("answer") the request which is waiting at the file descriptor accept() returned
                ServeRequest(acceptedConnection, acceptedConnection, mountPoints);

                // After we served the request, close the connection
                if (close(acceptedConnection) < 0)
                {
#ifdef __DEBUG__
                    printf("Error closing connection %d: %d %s\n", acceptedConnection, errno, strerror(errno));
#endif
                }
            }
            else if (errno == ECONNABORTED && isRunning)
            {
                // Make sure this only happens once
                isRunning = false;

                // Shutdown and close a socket if still open
                shutdown(serverSocket, SHUT_RDWR);
                close(serverSocket);

                // Start the sever again
                Start();
            }
            
        }
    }
    
}

void WebServer::ServeRequest(int in, int out, std::vector<const char*> mountPoints)
{
    // printf("serve request\n");
    // A lot of the code here is taken from the german page here: https://www.kompf.de/cplus/artikel/httpserv.html
    
    // Will hold the raw data to send out
    char buffer[8192];

    // Various pointers for the header reading
    char *b = buffer;
    char *l = buffer;
    char *le;

    // Counts the bytes received from a request
    int bytesReceived = 0;
    int bytesTotal = 0;

    // URL which was requested
    char url[256];
    *url = 0;

    // Will hold the requested URL mapped to the server filesystem
    char path[256];

    // Will hold the file descriptor of the file which was requested (e.g. a .html file)
    int fileToServe;

    // For the /gallery endpoint, this holds the page argument
    // int galleryPage = 0;

    // While there is data to receive, receive it
    // NOTE: recv() is a blocking call, see here: http://man7.org/linux/man-pages/man2/recv.2.html
    //       Therefore, the code may halt here to wait for incoming data. Some browsers, such as
    //       Google Chrome like to open a second socket (but not send anything) as a backup socket
    //       for optimization. Therefore, we would wait forever for data to arrive on that socket.
    //       That's why I set a timeout for recv calls() before, see WebServer::Start()
    bool breakReceive = false;
    while ((bytesReceived = recv(in, b, sizeof(buffer) - bytesTotal, 0)) > 0) 
    {
        // Count the bytes received
        bytesTotal += bytesReceived;
        b += bytesReceived;

        // Go through all bytes we received until now
        while (l < b) 
        {
            // Check if we reached the end of the request by looking at terminators
            le = l;
            while (le < b && *le != '\n' && *le != '\r')
            { 
                ++le;
            }

            if ('\n' == *le || '\r' == *le) 
            {
                // Right now we reached the end and therefore we have a full request header
                *le = 0;
                // printf("Header \"%s\"\n", l);

                // Scan the request header to see if this was the last request header
                // The last request header will always contain the operation requested, such as "GET /index.html HTTP/1.0"
                // The requested URL will be stored in url
                sscanf(l, "GET %255s HTTP/", url);

                // Was the last header we received an operation? If so, we reached the end
                // of all request headers and we can break this while loop
                if (strlen(l))
                    breakReceive = true;

                l = le + 1;
            }
        }

        // Should we break? Then break
        if (breakReceive) 
            break;
    }

    // Did the request contain a URL for us to serve?
    if (strlen(url) > 0) 
    {
#ifdef __DEBUG__
        printf("Received request: GET %s\n", url);
#endif
        // Map the requested URL to the path where we serve web assets
        // If the request didn't specify a file but only a "/", we route
        // to index.html aswell.
        if (strcmp(url, "/") == 0)
        {
            strcpy(url, "/index.html");
        }

        // Find the file in one of the mounted folders
        for (const char* mountPoint : mountPoints)
        {
            // Map the path of the requested file to this mount point
            sprintf(path, "%s%s", mountPoint, url);

            // Stat to see if the file exists at that mountpoint
            struct stat fileStat;
            if (stat(path, &fileStat) == 0)
            {
                // If so, break the loop as we have found our file path for the requested file
                break;
            }
        }

        // Open the requested file
        fileToServe = open(path, O_RDONLY);

        // Check if we file we tried to open existed
        if (fileToServe > 0) {
            // The file exists, so lets send an 200 OK to notify the client
            // that we will continue to send HTML data now
            sprintf(buffer, "HTTP/1.0 200 OK\n\n"); // \nContent-Type: text/html
            send(out, buffer, strlen(buffer), 0);

            // Read the data from the file requested until there is no data left to read
            do {
                bytesReceived = read(fileToServe, buffer, sizeof(buffer));

                // Send it out to the client
                send(out, buffer, bytesReceived, 0);
            } while (bytesReceived > 0);

            // We successfully read the file to serve, so close it
            close(fileToServe);
        }
        // No file to serve, check for the /gallery endpoint (and also parse out the arguments directly)
        // else if (sscanf(url, "/gallery?page=%d", &galleryPage))
        // {
        //     // First, send a 200 OK back
        //     sprintf(buffer, "HTTP/1.0 200 OK\nContent-Type: application/json\n\n");
        //     send(out, buffer, strlen(buffer), 0);

        //     // Ask the album wrapper to process the request
        //     std::string jsonData = nxgallery::AlbumWrapper::Get()->GetGalleryContent(galleryPage);

        //     // Send out the data to the socket
        //     const char* dataPtr = jsonData.data();
        //     std::size_t dataSize = jsonData.size();
        //     int bytesSent = 0;

        //     while (dataSize > 0)
        //     {
        //         bytesSent = send(out, dataPtr, dataSize, 0);
        //         if (bytesSent < 0)
        //             break;

        //         dataPtr += bytesSent;
        //         dataSize -= bytesSent;
        //     }
        // }
        // No file, and no endpoint will result in a 404
        else
        {
            // The requested file did not exist, send out a 404
            sprintf(buffer, "HTTP/1.0 404 Not Found\n\n");
            send(out, buffer, strlen(buffer), 0);
        }
    }
    // For the reason mentioned above, it might happen that we receive data which is
    // 0 bytes long. Make sure to ignore that and NOT return a 501 then
    else if (bytesTotal > 0)
    {
        // There was no URL given, likely that another request was issued.
        // Return a 501
        sprintf(buffer, "HTTP/1.0 501 Method Not Implemented\n\n");
        send(out, buffer, strlen(buffer), 0);
    }
}

void WebServer::Stop()
{
    // Not running anymore
    isRunning = false;

    // Shutdown the server socket, then close it
    shutdown(serverSocket, SHUT_RDWR);
    close(serverSocket);

#ifdef __DEBUG__
    printf("Stopped WebServer\n");
#endif
}
