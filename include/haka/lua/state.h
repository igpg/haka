
#ifndef _HAKA_LUA_STATE_H
#define _HAKA_LUA_STATE_H

#include <haka/types.h>

struct lua_State;

struct lua_state {
	struct lua_State    *L;
};


struct lua_state *lua_state_init();
void lua_state_close(struct lua_state *state);
bool lua_state_isvalid(struct lua_state *state);

int lua_state_error_formater(struct lua_State *L);
struct lua_state *lua_state_get(struct lua_State *L);

#endif /* _HAKA_LUA_STATE_H */