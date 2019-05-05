#!/usr/bin/env lua5.3

local pipe = require "pipe"

local p = pipe.open ("pacmd")
p:write ("dump\n")              -- Don't forget the \n
p:flush()
local res = p:read "*l"
print (res)
res = p:read "*l"
print (res)
res = p:read "*l"
print (res)
-- local res = p:read ()
-- print (res)


res = p:read "*l"
while res do
    print ("->", res)
    res = p:read "*l"
end
