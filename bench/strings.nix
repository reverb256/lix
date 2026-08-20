let
  strs = builtins.genList (i: builtins.toString i) 5000;
  joined = builtins.concatStringsSep "-" strs;
  replaced = builtins.replaceStrings [ "3" ] [ "III" ] joined;
  len = builtins.stringLength replaced;
  sub = builtins.substring 0 1000 replaced;
  parts = builtins.split "-" sub;
in
builtins.length parts
