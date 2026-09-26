local a = "Q\
R\z  
	S"
local b = [=[
X
Y
Z]=]
local joined = a .. "|" .. b
local bytes = {}
for i = 1, #joined do
    bytes[i] = string.format("%02X", string.byte(joined, i))
end
return table.concat(bytes, "")