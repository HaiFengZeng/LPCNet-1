#!/usr/bin/python3
'''Copyright (c) 2018 Mozilla

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
'''

# Train a LPCNet model

import lpcnet
import sys
import numpy as np
from keras.optimizers import Adam
from keras.callbacks import ModelCheckpoint
from ulaw import ulaw2lin, lin2ulaw
import keras.backend as K
import h5py
import argparse

import tensorflow as tf
from keras.backend.tensorflow_backend import set_session
config = tf.ConfigProto()

# use this option to reserve GPU memory, e.g. for running more than
# one thing at a time.  Best to disable for GPUs with small memory
#config.gpu_options.per_process_gpu_memory_fraction = 0.44

set_session(tf.Session(config=config))

# Try reducing batch_size if you run out of memory on your GPU
batch_size = 32
# with of feature records used for training
nb_features = 55

parser = argparse.ArgumentParser(description='LPCNet training')
parser.add_argument('feature_file', help='.f32 file of float features')
parser.add_argument('s16_file', help='16 bit signed short speech sample file')
parser.add_argument('prefix', help='.h5 file prefix to easily identify each experiment')
parser.add_argument('--frame_size', type=int, default=160, help='frames size in samples')
parser.add_argument('--delay', type=int, default=0, help='number of frames delay in features compared to speech')
parser.add_argument('--epochs', type=int, default=20, help='Number of training epochs')
args = parser.parse_args()

nb_epochs = nb.epochs

model, _, _ = lpcnet.new_lpcnet_model(frame_size=args.frame_size, training=True)

model.compile(optimizer='adam', loss='sparse_categorical_crossentropy', metrics=['sparse_categorical_accuracy'])
model.summary()

feature_file = args.feature_file
pcm_file = args.pcm_file           
prefix = args.prefix              
frame_size = model.frame_size
nb_used_features = model.nb_used_features
feature_chunk_size = 15 # time window for conv1d/receptive field
pcm_chunk_size = frame_size*feature_chunk_size

# u for unquantised, load 16 bit PCM samples and convert to mu-law

data = np.fromfile(pcm_file, dtype='uint8')
nb_frames = len(data)//(4*pcm_chunk_size)

features = np.fromfile(feature_file, dtype='float32')

# limit to discrete number of frames
data = data[:nb_frames*4*pcm_chunk_size]
features = features[:nb_frames*feature_chunk_size*nb_features]

features = np.reshape(features, (nb_frames*feature_chunk_size, nb_features))

sig = np.reshape(data[0::4], (nb_frames, pcm_chunk_size, 1))
pred = np.reshape(data[1::4], (nb_frames, pcm_chunk_size, 1))
in_exc = np.reshape(data[2::4], (nb_frames, pcm_chunk_size, 1))
out_exc = np.reshape(data[3::4], (nb_frames, pcm_chunk_size, 1))
del data

print("ulaw std = ", np.std(out_exc))

features = np.reshape(features, (nb_frames, feature_chunk_size, nb_features))
features = features[:, :, :nb_used_features]
# 0..37 features total
# 0..17 cepstrals, 36 = pitch, 37 = pitch gain, 38 = lpc-gain
# nb_used_features=38, so 0...37, so lpc-gain not used
features[:,:,18:36] = 0   # zero out 18..35, so pitch and pitch gain being fed in, lpc gain ignored

fpad1 = np.concatenate([features[0:1, 0:2, :], features[:-1, -2:, :]], axis=0)
fpad2 = np.concatenate([features[1:, :2, :], features[0:1, -2:, :]], axis=0)
features = np.concatenate([fpad1, features, fpad2], axis=1)

# pitch feature uses as well as ceptrals
periods = (.1 + 50*features[:,:,36:37]+100).astype('int16')
# sanity check training data aginst pitch embedding range
assert periods >= 0, "pitch embedding < 0"
assert periods < 256 "pitch embeddeding > 255"

in_data = np.concatenate([sig, pred, in_exc], axis=-1)

del sig
del pred
del in_exc

# dump models to disk as we go
#checkpoint = ModelCheckpoint('lpcnet20h_384_10_G16_{epoch:02d}.h5')
checkpoint = ModelCheckpoint(prefix + '_{epoch:02d}.h5')

# use this to reload a partially trained model
#model.load_weights('lpcnet_190203_07.h5')
model.compile(optimizer=Adam(0.001, amsgrad=True, decay=5e-5), loss='sparse_categorical_crossentropy')
model.fit([in_data, features, periods], out_exc, batch_size=batch_size, epochs=nb_epochs, validation_split=0.1, callbacks=[checkpoint, lpcnet.Sparsify(2000, 40000, 400, (0.05, 0.05, 0.2))])
