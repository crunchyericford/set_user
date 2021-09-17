FROM postgres:9.4
RUN \
apt-get update && apt-get -y upgrade && \
apt-get -y install git postgresql-server-dev-9.4 make gcc && \
git clone --branch cicd https://github.com/crunchyericford/set_user /opt/set_user && \
make -C /opt/set_user USE_PGXS=1 && make -C /opt/set_user USE_PGXS=1 install
