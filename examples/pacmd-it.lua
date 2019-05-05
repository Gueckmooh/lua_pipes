local pipe = require "pipe"

local p = pipe.open "pacmd"
p:write "dump\n"

for line in p:lines () do
    -- do something with line
    print (line)
end
