# Multi-Modal Model for Recommender Systems
from collections import Counter
import requests
import json

import networkx as nx
import numpy as np

import ukv.umem as ukv


def flatten(l):
    return [item for sublist in l for item in sublist]


def fill_db(all_follows, all_views, all_posters, all_people, all_movies):
    jds = json.dumps
    def load(x): return requests.get(x).content

    alice, bob, charlie, david = 1, 2, 3, 4
    all_people.set((alice, bob, charlie, david), (
        jds({'name': 'Alice', 'lastname': 'Bobchinsky', 'stars': 1200}),
        jds({'name': 'Bob', 'lastname': 'Charleston', 'stars': 700}),
        jds({'name': 'Charlie', 'lastname': 'Allison', 'stars': 800}),
        jds({'name': 'David', 'lastname': 'Davidson', 'stars': 500})
    ))
    all_movies.set(range(101, 110), (
        jds({'title': 'The Fast and the Furious', 'rating': 6.8}),
        jds({'title': '2 Fast 2 Furious', 'rating': 5.9}),
        jds({'title': 'The Fast and the Furious: Tokyo Drift', 'rating': 6}),
        jds({'title': 'Fast & Furious', 'rating': 6.5}),
        jds({'title': 'Fast Five', 'rating': 7.3}),
        jds({'title': 'Fast & Furious 6', 'rating': 7}),
        jds({'title': 'Furious 7', 'rating': 7.1}),
        jds({'title': 'The Fate of the Furious', 'rating': 6.6}),
        jds({'title': 'F9', 'rating': 5.2})
    ))

    all_posters.set(range(101, 110), (
        load('https://upload.wikimedia.org/wikipedia/en/5/54/Fast_and_the_furious_poster.jpg'),
        load('https://upload.wikimedia.org/wikipedia/en/9/9d/Two_fast_two_furious_ver5.jpg'),
        load('https://upload.wikimedia.org/wikipedia/en/4/4f/Poster_-_Fast_and_Furious_Tokyo_Drift.jpg'),
        load('https://upload.wikimedia.org/wikipedia/en/8/8f/Fast_and_Furious_Poster.jpg'),
        load('https://upload.wikimedia.org/wikipedia/en/0/0c/Fast_Five_poster.jpg'),
        load('https://upload.wikimedia.org/wikipedia/en/3/30/Fast_%26_Furious_6_film_poster.jpg'),
        load('https://upload.wikimedia.org/wikipedia/en/b/b8/Furious_7_poster.jpg'),
        load('https://upload.wikimedia.org/wikipedia/en/2/2d/The_Fate_of_The_Furious_Theatrical_Poster.jpg'),
        load('https://upload.wikimedia.org/wikipedia/en/2/2b/F9_film_poster.jpg')
    ))

    # Alice only watches even movies
    all_views.add_edge(alice, 102)
    all_views.add_edge(alice, 104)
    all_views.add_edge(alice, 106)
    all_views.add_edge(alice, 108)
    # Bob has only watched the middle of the series
    all_views.add_edge(bob, 103)
    all_views.add_edge(bob, 104)
    all_views.add_edge(bob, 105)
    all_views.add_edge(bob, 107)
    # Charlie only watched the first 5
    all_views.add_edge(charlie, 101)
    all_views.add_edge(charlie, 102)
    all_views.add_edge(charlie, 103)
    all_views.add_edge(charlie, 104)
    all_views.add_edge(charlie, 105)

    all_follows.add_edge(alice, bob)
    all_follows.add_edge(david, alice)
    all_follows.add_edge(david, bob)
    all_follows.add_edge(david, charlie)


def test():
    db = ukv.DataBase()

    all_follows = db['people->people'].graph
    all_views = db['people->movies'].graph
    all_posters = db['movies.poster'].media
    all_people = db['people'].docs
    all_movies = db['movies'].docs
    fill_db(all_follows, all_views, all_posters, all_people, all_movies)

    # Let's recommend this user some movies.
    # For that:
    # 1. Find all the people our user follows
    # 2. Sample all the movies they have watched
    # 3. Remove the movies that our customer has watched
    # 4. Rank movies by appearance frequency
    user_id = 10
    follows = all_follows.successors(user_id)
    potential_movies = all_views.successors(follows)
    movies_rank = Counter(flatten(potential_movies))
    for watched_movie in all_views.successors(user_id):
        movies_rank.pop(watched_movie)
    common_movies = [x[0] for x in movies_rank.most_common(10)]

    # As we have picked the movies, let's build up the bipartite graph
    nx.draw_networkx(
        all_views.subgraph(),
        pos=nx.drawing.layout.bipartite_layout(B, follows),
        labels={})

    # As we have picked the movies, let's build up the bipartite graph
    # For that:
    # 1. Get the metadata
    # 2. Build-up a NetworkX graph
    people_sample = all_people[follows][['name', 'lastname', 'stars']]
    movies_sample = all_movies[common_movies][['title', 'rating']]
    posters_sample = all_posters[common_movies]

    # Let's assume the user watched the recommended movie,
    # didn't like it and wants to unfollow everyone who recommended it...
    # in a single atomic transaction:
    bad_movie_id = 0
    bad_influencers = [follows]
    with db.transact() as txn:
        txn['people->movies'].graph.add_edge(user_id, bad_movie_id)
        txn['people->people'].graph.remove_edges_from(
            [user_id]*len(follows),
            bad_influencers
        )

    # Let's assume, a new video is being uploaded
    new_movie_id = 100
    with db.transact() as txn:
        txn['movies'].docs[new_movie_id] = {
            'title': 'Fast & Extremely Furious 100',
            'text': 'Dominic is racing on rockets in space.',
            'rating': 5,
        }
        txn['movies.poster'] = open('fast100.jpeg', 'rb').read()
