# turns the files in web/ into one header, so the binary needs no other files
set(out "#pragma once\n#include<string>\n")
foreach(name index.html style.css app.js favicon.svg)
    file(READ "${WEB_DIR}/${name}" content)
    string(REPLACE "." "_" id "${name}")
    string(APPEND out "inline const std::string web_${id} = R\"web_file(${content})web_file\";\n")
endforeach()
file(WRITE "${OUT_FILE}" "${out}")
