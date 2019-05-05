local pipe = require "pipe"

local p = pipe.open ("pacmd")
p:write ("dump\n")              -- Don't forget the \n
local res = p:read ()
print (res)
