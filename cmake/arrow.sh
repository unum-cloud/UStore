# Suggestions: https://arrow.apache.org/install/
# https://anaconda.org/fastchan/arrow-cpp/files?sort=basename&version=9.0.0&sort_order=desc
# https://anaconda.org/fastchan/pyarrow/files?sort=basename&version=9.0.0&sort_order=desc
# conda install pyarrow=9.0.0=py39h42d110c_0_cpu arrow-cpp=9.0.0=py39h811ffd7_0_cpu -c fastchan
# conda install arrow-cpp=9.0.0=py39h811ffd7_0_cpu -c fastchan

export arrow_version=9.0.0-1 &&
    sudo apt update && sudo apt install -y -V ca-certificates lsb-release wget &&
    cd /tmp && wget https://apache.jfrog.io/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb &&
    sudo apt install -y -V ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb &&
    sudo apt update && sudo apt install -y -V libarrow-dev=${arrow_version} libarrow-dataset-dev=${arrow_version} libarrow-flight-dev=${arrow_version} \
    libarrow-python-dev=${arrow_version} libarrow-python-flight-dev=${arrow_version} libgandiva-dev=${arrow_version} libparquet-dev=${arrow_version} \
    python3.9 python3-pip python3-dbg python3.9-dev
