# Fix C++ keyword 'namespace' in generated Wayland protocol header
file(READ "${HEADER}" content)
string(REPLACE "*namespace)" "*namespace_)" content "${content}")
string(REPLACE ", namespace)" ", namespace_)" content "${content}")
file(WRITE "${HEADER}" "${content}")
