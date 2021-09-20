ARG CONT_IMG_VER
ARG POSTGRES_SERVER_DEV_VER
FROM postgres:${CONT_IMG_VER}
RUN apt-get update && apt-get -y upgrade && \
apt-get -y install git ${POSTGRES_SERVER_DEV_VER} make gcc && \
git clone --branch cicd https://github.com/crunchyericford/set_user /opt/set_user && \
make -C /opt/set_user USE_PGXS=1 && make -C /opt/set_user USE_PGXS=1 install
