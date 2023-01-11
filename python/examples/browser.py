import requests as re

import ukv.umem as ukv
import streamlit as st

collection_name_docs = 'Docs'
collection_name_network = 'Network'
collection_name_images = 'Images'


@st.experimental_singleton
def get_database_session(url=None):
    db = ukv.DataBase()
    col_docs = db[collection_name_docs].docs
    col_network = db[collection_name_network].graph
    col_images = db[collection_name_images]
    if True or not len(col_docs):
        col_docs[10] = {'a': 10, 'b': 20}
    if True or len(col_images):
        links = [
            'https://upload.wikimedia.org/wikipedia/commons/thumb/8/87/Mount_Ararat_and_the_Yerevan_skyline_in_spring_from_the_Cascade.jpg/556px-Mount_Ararat_and_the_Yerevan_skyline_in_spring_from_the_Cascade.jpg',
            'https://upload.wikimedia.org/wikipedia/commons/thumb/3/31/Vartavar_2014_Yerevan_%285%29.jpg/202px-Vartavar_2014_Yerevan_%285%29.jpg',
            'https://upload.wikimedia.org/wikipedia/commons/thumb/6/6e/Cascade_of_Yerevan.JPG/168px-Cascade_of_Yerevan.JPG',
            'https://upload.wikimedia.org/wikipedia/commons/thumb/6/64/Yerevan_Tsitsernakaberd_Armenian_Genocide_Museum_Memorial_msu-2018-3009.jpg/170px-Yerevan_Tsitsernakaberd_Armenian_Genocide_Museum_Memorial_msu-2018-3009.jpg',
            'https://upload.wikimedia.org/wikipedia/commons/thumb/1/19/%C3%93pera%2C_Erev%C3%A1n%2C_Armenia%2C_2016-10-03%2C_DD_12.jpg/230px-%C3%93pera%2C_Erev%C3%A1n%2C_Armenia%2C_2016-10-03%2C_DD_12.jpg',
            'https://upload.wikimedia.org/wikipedia/commons/thumb/d/dc/Jerewan-Matenadaran-msu-wlm-2509.jpg/140px-Jerewan-Matenadaran-msu-wlm-2509.jpg'
        ]
        for idx, url in enumerate(links):
            col_images[50+idx] = re.get(url).content
    if True or len(col_network):
        pass
    return db


option = st.selectbox(
    'Which collection would you like to view?',
    (collection_name_docs, collection_name_images, collection_name_network))

st.write('You selected:', option)

if option == collection_name_docs:

    for key, value in get_database_session()[collection_name_docs].docs.items:
        st.json(value)
