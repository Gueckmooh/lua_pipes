g = require "pipe"

-- l = g.popen2("cat")
-- for v, k in pairs (l) do print (v, k) end

l = g.new ("pacmd")

l:write ("dump\n")

l:read ()

l:close ()
