# prepend the LICENSE as a comment block: OUT = /* LICENSE */ + BODY
file(READ "${LICENSE}" license)
file(READ "${BODY}" body)
file(WRITE "${OUT}" "/*\n${license}*/\n${body}")
