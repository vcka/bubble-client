#ifndef __UTILS_H__
#define __UTILS_H__

#include <cstdio>

inline void LOG_BUFFER_HEX(const char *msg, int nbytes, const char *buffer)
{
	std::printf("%s\n", msg);
	for (int i = 0; i < nbytes; i++)
	{
		std::printf("%02x ", (unsigned char)buffer[i]);
	}
	printf("\n");
}

#endif
