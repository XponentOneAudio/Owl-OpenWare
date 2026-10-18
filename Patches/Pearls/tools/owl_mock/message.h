// OwlProgram's message.h, for the desktop tests: keeps the last message.
#ifndef OWL_MOCK_MESSAGE_H_
#define OWL_MOCK_MESSAGE_H_

#include <cstring>

inline char* LastDebugMessage() {
  static char message[64];
  return message;
}

inline void debugMessage(const char* msg) {
  strncpy(LastDebugMessage(), msg, 63);
}

#endif  // OWL_MOCK_MESSAGE_H_
