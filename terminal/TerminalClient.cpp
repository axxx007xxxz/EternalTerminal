#include "ClientConnection.hpp"
#include "CryptoHandler.hpp"
#include "FlakyFakeSocketHandler.hpp"
#include "Headers.hpp"
#include "NCursesOverlay.hpp"
#include "ProcessHelper.hpp"
#include "ServerConnection.hpp"
#include "SocketUtils.hpp"
#include "UnixSocketHandler.hpp"

#include <errno.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>

#include "ETerminal.pb.h"

using namespace et;
shared_ptr<ClientConnection> globalClient;

#define FAIL_FATAL(X)                                    \
  if ((X) == -1) {                                       \
    printf("Error: (%d), %s\n", errno, strerror(errno)); \
    exit(errno);                                         \
  }

termios terminal_backup;

DEFINE_string(host, "localhost", "host to join");
DEFINE_int32(port, 10022, "port to connect on");
DEFINE_string(passkey, "", "Passkey to encrypt/decrypt packets");
DEFINE_string(passkeyfile, "", "Passkey file to encrypt/decrypt packets");

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  FLAGS_logbufsecs = 0;
  FLAGS_logbuflevel = google::GLOG_INFO;
  srand(1);

  std::shared_ptr<SocketHandler> clientSocket(new UnixSocketHandler());

  string passkey = FLAGS_passkey;
  if (passkey.length() == 0 && FLAGS_passkeyfile.length() > 0) {
    // Check for passkey file
    std::ifstream t(FLAGS_passkeyfile.c_str());
    std::stringstream buffer;
    buffer << t.rdbuf();
    passkey = buffer.str();
    // Trim whitespace
    passkey.erase(passkey.find_last_not_of(" \n\r\t") + 1);
    // Delete the file with the passkey
    remove(FLAGS_passkeyfile.c_str());
  }
  if (passkey.length() == 0) {
    cout << "Unless you are doing development on Eternal Terminal,\nplease do "
            "not call etclient directly.\n\nThe et launcher (run on the "
            "client) calls etclient with the correct parameters.\nThis ensures "
            "a secure connection.\n\nIf you intended to call etclient "
            "directly, please provide a passkey\n(run \"etclient --help\" for "
            "details)."
         << endl;
    exit(1);
  }
  if (passkey.length() != 32) {
    LOG(FATAL) << "Invalid/missing passkey: " << passkey << " "
               << passkey.length();
  }

  InitialPayload payload;
  winsize win;
  ioctl(1, TIOCGWINSZ, &win);
  TerminalInfo* ti = payload.mutable_terminal();
  ti->set_row(win.ws_row);
  ti->set_column(win.ws_col);
  ti->set_width(win.ws_xpixel);
  ti->set_height(win.ws_ypixel);
  char* term = getenv("TERM");
  if (term) {
    LOG(INFO) << "Sending command to set terminal to " << term;
    // Set terminal
    string s = std::string("TERM=") + std::string(term);
    payload.add_environmentvar(s);
  }

  shared_ptr<ClientConnection> client = shared_ptr<ClientConnection>(
      new ClientConnection(clientSocket, FLAGS_host, FLAGS_port, passkey));
  globalClient = client;
  int connectFailCount = 0;
  while (true) {
    try {
      client->connect();
      client->writeProto(payload);
    } catch (const runtime_error& err) {
      LOG(ERROR) << "Connecting to server failed: " << err.what();
      connectFailCount++;
      if (connectFailCount == 3) {
        LOG(INFO) << "Could not make initial connection to server";
        cout << "Could not make initial connection to " << FLAGS_host << ": "
             << err.what() << endl;
        exit(1);
      }
      continue;
    }
    break;
  }
  VLOG(1) << "Client created with id: " << client->getClientId() << endl;

  termios terminal_local;
  tcgetattr(0, &terminal_local);
  memcpy(&terminal_backup, &terminal_local, sizeof(struct termios));
  cfmakeraw(&terminal_local);
  tcsetattr(0, TCSANOW, &terminal_local);

  // Whether the TE should keep running.
  bool run = true;

// TE sends/receives data to/from the shell one char at a time.
#define BUF_SIZE (1024)
  char b[BUF_SIZE];

  time_t keepaliveTime = time(NULL) + 5;
  bool waitingOnKeepalive = false;

  unique_ptr<NCursesOverlay> disconnectedOverlay;
#if 0
  string offlineBuffer;
#endif
  while (run) {
#if 0  // This doesn't work with tmux and when combined with a curses
      // app on the server side causes weird graphical glitches.

    // TODO: Figure out why this causes issues.
    if (disconnectedOverlay.get()==NULL && globalClient->isDisconnected()) {
      disconnectedOverlay.reset(new NCursesOverlay());
      shared_ptr<NCursesWindow> popupWindow;
      {
        TerminalInfo terminfo;
        terminfo.set_id("popup");
        terminfo.set_height(7);
        terminfo.set_width(41);
        terminfo.set_row(disconnectedOverlay->rows()/2 - 3);
        terminfo.set_column(disconnectedOverlay->cols()/2 - 20);

        popupWindow = disconnectedOverlay->createWindow(terminfo, true);
        popupWindow->drawTextCentered(FLAGS_host, 1);
        popupWindow->drawTextCentered("Connection lost.", 3);
        popupWindow->drawTextCentered("Please wait...", 5);
      }
      disconnectedOverlay->refresh();
    }
    if (disconnectedOverlay.get() && !globalClient->isDisconnected()) {
      disconnectedOverlay.reset(NULL);
      FATAL_FAIL(writeAll(STDOUT_FILENO, &offlineBuffer[0], offlineBuffer.length()));
    }
#endif
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    fd_set wfd;
    fd_set efd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_ZERO(&wfd);
    FD_ZERO(&efd);
    FD_SET(STDIN_FILENO, &rfd);
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    select(STDIN_FILENO + 1, &rfd, &wfd, &efd, &tv);

    try {
      // Check for data to send.
      if (FD_ISSET(STDIN_FILENO, &rfd)) {
        // Read from stdin and write to our client that will then send it to the
        // server.
        int rc = read(STDIN_FILENO, b, BUF_SIZE);
        FAIL_FATAL(rc);
        if (rc > 0) {
          // VLOG(1) << "Sending byte: " << int(b) << " " << char(b) << " " <<
          // globalClient->getWriter()->getSequenceNumber();
          string s(b, rc);
          et::TerminalBuffer tb;
          tb.set_buffer(s);

          char c = et::PacketType::TERMINAL_BUFFER;
          string headerString(1, c);
          globalClient->writeMessage(headerString);
          globalClient->writeProto(tb);
          keepaliveTime = time(NULL) + 5;
        } else {
          LOG(FATAL) << "Got an error reading from stdin: " << rc;
        }
      }

      while (globalClient->hasData()) {
        string packetTypeString;
        if (!globalClient->readMessage(&packetTypeString)) {
          break;
        }
        if (packetTypeString.length() != 1) {
          LOG(FATAL) << "Invalid packet header size: "
                     << packetTypeString.length();
        }
        char packetType = packetTypeString[0];
        switch (packetType) {
          case et::PacketType::TERMINAL_BUFFER: {
            // Read from the server and write to our fake terminal
            et::TerminalBuffer tb =
                globalClient->readProto<et::TerminalBuffer>();
            const string& s = tb.buffer();
            // VLOG(1) << "Got byte: " << int(b) << " " << char(b) << " " <<
            // globalClient->getReader()->getSequenceNumber();
            keepaliveTime = time(NULL) + 1;
#if 0
            if (disconnectedOverlay.get()) {
              offlineBuffer += s;
            } else {
#endif
            FATAL_FAIL(writeAll(STDOUT_FILENO, &s[0], s.length()));
#if 0
            }
#endif
            break;
          }
          case et::PacketType::KEEP_ALIVE:
            waitingOnKeepalive = false;
            break;
          default:
            LOG(FATAL) << "Unknown packet type: " << int(packetType) << endl;
        }
      }

      if (keepaliveTime < time(NULL)) {
        keepaliveTime = time(NULL) + 5;
        if (waitingOnKeepalive) {
          LOG(INFO) << "Missed a keepalive, killing connection.";
          globalClient->closeSocket();
          waitingOnKeepalive = false;
        } else {
          VLOG(1) << "Writing keepalive packet";
          string s(1, (char)et::PacketType::KEEP_ALIVE);
          globalClient->writeMessage(s);
          waitingOnKeepalive = true;
        }
      }

      winsize tmpwin;
      ioctl(1, TIOCGWINSZ, &tmpwin);
      if (win.ws_row != tmpwin.ws_row || win.ws_col != tmpwin.ws_col ||
          win.ws_xpixel != tmpwin.ws_xpixel ||
          win.ws_ypixel != tmpwin.ws_ypixel) {
        win = tmpwin;
        LOG(INFO) << "Window size changed: " << win.ws_row << " " << win.ws_col
                  << " " << win.ws_xpixel << " " << win.ws_ypixel << endl;
        TerminalInfo ti;
        ti.set_row(win.ws_row);
        ti.set_column(win.ws_col);
        ti.set_width(win.ws_xpixel);
        ti.set_height(win.ws_ypixel);
        string s(1, (char)et::PacketType::TERMINAL_INFO);
        globalClient->writeMessage(s);
        globalClient->writeProto(ti);
      }

    } catch (const runtime_error& re) {
      LOG(ERROR) << "Error: " << re.what() << endl;
      cerr << "Error: " << re.what() << endl;
      run = false;
    }

    usleep(1000);
  }

  disconnectedOverlay.release();
  tcsetattr(0, TCSANOW, &terminal_backup);
  globalClient.reset();
  client.reset();
  LOG(INFO) << "Client derefernced" << endl;
  return 0;
}
