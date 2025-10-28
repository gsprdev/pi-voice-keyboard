# Expose /tmp/kb.sock via TCP 1234 for easier use in SSH tunneling
sudo socat TCP-LISTEN:1234,reuseaddr,fork UNIX-CONNECT:/tmp/kb.sock 
