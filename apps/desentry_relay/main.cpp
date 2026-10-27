#include <cstdio>
#include <iostream>
#include <string>

#include "desentry/common/logger.h"
#include "desentry/common/platform.h"

int main(int argc, char** argv) {
  desentry::NetInit();
  std::printf("desentry-relay: minimal WebSocket relay server (metadata-only broker)\n");
  std::printf("This is a stub implementation. Full WebSocket relay server is future work.\n");
  return 0;
}
