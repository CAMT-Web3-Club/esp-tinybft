{
  description = "ESP-IDF Full Development Environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    esp-dev.url = "github:mirrexagon/nixpkgs-esp-dev";
    esp-dev.inputs.nixpkgs.follows = "nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    { nixpkgs, esp-dev, ... }:
    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;

      nixpkgsFor =
        system:
        import nixpkgs {
          inherit system;
          overlays = [ esp-dev.overlays.default ];
          config = {
            allowUnfree = true;
            # The Python library ecdsa is marked as insecure, but we need it for esptool.
            # See https://github.com/mirrexagon/nixpkgs-esp-dev/issues/109
            permittedInsecurePackages = [
              "python3.13-ecdsa-0.19.1"
            ];
          };
        };
    in
    {
      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgsFor system;
        in
        {
          default = pkgs.mkShell {
            name = "esp-idf-full-shell";

            buildInputs = with pkgs; [
              esp-idf-full
              esptool
              cmake
              ninja
            ];
            shellHook = ''
              export IDF_PATH=${pkgs.esp-idf-full}
              alias idf.py='python $IDF_PATH/tools/idf.py'
              echo "--- 🛠️  ESP-IDF Fix Applied 🛠️  ---"
              echo "IDF_PATH: $IDF_PATH"
              echo "-------------------------------------------"
            '';
          };
        }
      );
    };
}
