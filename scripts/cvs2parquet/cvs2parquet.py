import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq


def csv_to_parquet(csv_file_path, parquet_file_path):
    """
    Convert CSV to Parquet with specific column types:
    - stage: int32
    - block: int32
    - g1: float64 (double)
    """

    # Read CSV file
    df = pd.read_csv(csv_file_path)

    # Convert column types
    df['stage'] = df['stage'].astype('int32')
    df['block'] = df['block'].astype('int32')
    df['g1'] = df['g1'].astype('float64')  # double precision

    # Save as Parquet
    df.to_parquet(parquet_file_path, index=False)

    print(f"Successfully converted {csv_file_path} to {parquet_file_path}")
    print(f"Data types: {df.dtypes}")

# Alternative method using PyArrow for more control over schema


def csv_to_parquet_with_schema(csv_file_path, parquet_file_path):
    """
    Convert CSV to Parquet using PyArrow with explicit schema definition
    """

    # Read CSV
    df = pd.read_csv(csv_file_path)

    # Define explicit schema
    schema = pa.schema([
        ('stage', pa.int32()),
        ('block', pa.int32()),
        ('g1', pa.float64())
    ])

    # Convert to PyArrow table with schema
    table = pa.Table.from_pandas(df[['stage', 'block', 'g1']], schema=schema)

    # Write to Parquet
    pq.write_table(table, parquet_file_path)

    print(f"Successfully converted {csv_file_path} to {parquet_file_path}")
    print(f"Schema: {table.schema}")


# Usage examples
if __name__ == "__main__":
    # Method 1: Using pandas
    csv_to_parquet('input_data.csv', 'output_data.parquet')

    # Method 2: Using PyArrow with explicit schema
    # csv_to_parquet_with_schema('input_data.csv', 'output_data.parquet')

    # Verify the conversion
    df_check = pd.read_parquet('output_data.parquet')
    print("\nVerification:")
    print(f"Shape: {df_check.shape}")
    print(f"Data types:\n{df_check.dtypes}")
    print(f"First 5 rows:\n{df_check.head()}")
    print(f"First 5 rows:\n{df_check.head()}")
