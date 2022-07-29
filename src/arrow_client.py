import pyarrow as pa
import pyarrow.flight as pf
import pandas as pd

# https://arrow.apache.org/docs/python/generated/pyarrow.flight.FlightClient.html?highlight=flightclient#pyarrow.flight.FlightClient
connection: pf.FlightClient = pf.connect('grpc://0.0.0.0:38709')

assert len(list(connection.list_flights())) <= 1, 'Must be empty'


df_in = pd.DataFrame([{'a': 10, 'b': 11}, {'a': 2, 'c': 12}])

# Either command or filename
descriptor = pf.FlightDescriptor.for_path('some_name.parquet')

# Inferring schema from Pandas
# https://arrow.apache.org/docs/python/generated/pyarrow.Schema
schema: pa.Schema = pa.Schema.from_pandas(df_in) if len(df_in) else pa.schema([
    ('a', pa.int32()),
    ('str', pa.string())
])
options = pf.FlightCallOptions()

handle = connection.do_put(descriptor, schema, options)
writer: pf.FlightStreamWriter = handle[0]
reader: pf.FlightMetadataReader = handle[1]

# Write data into the stream:
# https://arrow.apache.org/docs/python/generated/pyarrow.flight.FlightStreamWriter.html?highlight=flightstreamwriter
table_in = pa.Table.from_pandas(df_in)
writer.write_table(table_in)
writer.done_writing()

# Fetch the responses
# https://arrow.apache.org/docs/python/generated/pyarrow.flight.FlightStreamReader
print('Received write reply with metadata', reader.read())
writer.close()

# Check which columns are available
# https://arrow.apache.org/docs/python/generated/pyarrow.flight.FlightClient.html?highlight=flightclient#pyarrow.flight.FlightClient.get_schema
schema = connection.get_schema(descriptor).read()


# Retrieve the entire table and check if the contents are matching
df_out = connection.do_get(pf.Ticket('some_name.parquet')).read_pandas()
table_out = connection.do_get(pf.Ticket('some_name.parquet')).read_all()
assert df_in.equals(table_out.to_pandas()), 'Returned table has changed'

exit()
