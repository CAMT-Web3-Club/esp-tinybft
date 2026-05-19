ARG DOCKER_TAG=latest
FROM espressif/idf:${DOCKER_TAG}

ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8

RUN DEBIAN_FRONTEND=noninteractive apt-get update -y && DEBIAN_FRONTEND=noninteractive apt-get install udev fish -y

RUN echo "source /opt/esp/idf/export.sh > /dev/null 2>&1" >> ~/.bashrc

RUN mkdir -p ~/.config/fish && cat >> ~/.config/fish/config.fish <<'EOF'
if status is-interactive
    bash -c '. /opt/esp/idf/export.sh >/dev/null 2>&1; env -0' | while read -lz -d '' line
        set -l key (string split -m 1 '=' -- $line)[1]
        set -l val (string split -m 1 '=' -- $line)[2..-1]
        if test -z "$key"; continue; end
        switch $key
            case 'BASH_FUNC*' 'SHLVL' 'PWD' 'OLDPWD' '_'
            case 'PATH'
                set -gx PATH (string split ':' -- $val)
            case '*'
                set -gx $key $val
        end
    end
end
EOF

ENV SHELL=/usr/bin/fish

ENTRYPOINT [ "/opt/esp/entrypoint.sh" ]

CMD ["/usr/bin/fish"]
