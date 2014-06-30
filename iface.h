/*
Copyright (c) 2014 David Hubbard

This program is free software: you can redistribute it and/or modify it under the terms of
the GNU Affero General Public License version 3, as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU Affero General Public License version 3 for more details.

You should have received a copy of the GNU Affero General Public License version 3 along with
this program.  If not, see <http://www.gnu.org/licenses/>.
*/

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned u32;

typedef int (* foreach_if_cb)(const char * if_name, u32 ip_addr, u32 netmask, void * ctx);
int foreach_if(int sock_to_kernel, foreach_if_cb cb, void * ctx);
#define ip_printf(s, ip) __ip_printf(s, sizeof(s), ip, __PRETTY_FUNCTION__, __LINE__)
void __ip_printf(char * s, size_t len, u32 ip, const char * funcname, unsigned long line);

int get_hw_addr(int sock_to_kernel, const char * ifname, u8 hwaddr[6]);
