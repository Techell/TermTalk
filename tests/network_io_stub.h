#ifndef WINSOCK_NETWORK_IO_STUB_H
#define WINSOCK_NETWORK_IO_STUB_H
/* Закрытый интерфейс тестовой сборки network.c; в приложения не включается. */
int NetworkTest_Send(SOCKET socket, const char *buffer, int length, int flags);
int NetworkTest_Recv(SOCKET socket, char *buffer, int length, int flags);
#endif
