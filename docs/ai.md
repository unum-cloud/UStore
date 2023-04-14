# UStore Usecases in AI 

Databases and AI have been largely unrelated.
In 80s, on the rise of symbolic systems, people called primitive Knowledge Graphs AI.
Today, the AI industry revolves around gigantic Neural Networks.
Here are the IO-intensive AI scenarios where UStore can be useful.

#### Larger Training Datasets

When you are training on a huge datasets, you may not even be able to load all the training metadata into RAM, to shuffle the samples, and schedule the training epoch.
For such cases we have added "sampling", as a first class citizen of UStore, on par with "get" and "set" operations common to any associative container.

> UStore is a good choice for larger Pre-Training tasks thanks to builtin "sampling" support.

#### Recommendation Systems

DLRM, and Recommender Networks in general, are famously the most IO-intensive Neural Networks.
On every recommendation cycle, they perform countless vector lookups, to perform the ranking.

> UStore is a good choice for Recommendation Systems thanks to high retrieval throughput.

#### Dialogue Systems

Modern dialogue systems are shifting towards retrieval-based generation, meaning that they don't extrapolate on their original training data, but instead use the information acquired from external sources.
To find relevant information, embeddings (vector-representations) are compared against database entries.

> UStore is a good choice for Dialogue Systems thanks to in-built vector search.

#### Semantic Search

Semantic Search and Knowledge Management platforms generally require a combination of pre-trained encoder models, vector search, a document database for metadata, and an object store for media content.
A good example would be marketplace like eBay.com, which, aside from the neural network itself, may need:

- Pinecone to store embeddings.
- MongoDB to store product descriptions.
- MinIO to store product pictures.

> UStore is a good choice for Semantic Search, because everything can fit in one box, simplifying usage and maintenance.

```python
id = hash('iphone')
image = open('iphone.png').read()
vec = vectorize(image)

with db.transaction() as db:
    db['posters'].blobs[id] = image
    db['embeddings'].vectors[id] = vec
    db['products'].docs[id] = {
        'title': 'iPhone',
        'price': 999 }
```